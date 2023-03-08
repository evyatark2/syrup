#include "map.h"

#include <stdio.h>
#include <stdlib.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "scripting/reactor-manager.h"
#include "client.h"
#include "server.h"
#include "drops.h"
#include "../hash-map.h"
#include "../packet.h"
#include "../wz.h"

struct MapPlayer {
    struct MapHandleContainer *container;
    struct ControllerHeapNode *node;
    size_t monsterCount;
    struct Monster **monsters;
    size_t dropCapacity;
    size_t dropCount;
    struct DropBatch **drops;
    size_t droppingCapacity;
    size_t droppingCount;
    struct DroppingBatch **droppings;
    struct ReactorManager *rm;
    struct ScriptInstance *script;
    struct Client *client;
};

struct Spawner {
    uint32_t id;
    int16_t x;
    int16_t y;
    int16_t fh;
};

struct Monster {
    uint32_t oid;
    uint32_t id;
    size_t spawnerIndex;
    struct MapPlayer *controller;
    size_t indexInController;
    int16_t x;
    int16_t y;
    uint16_t fh;
    uint8_t stance;
    int32_t hp;
};

struct Reactor {
    uint32_t oid;
    uint32_t id;
    int16_t x;
    int16_t y;
    uint8_t state;
    bool keepAlive;
};

enum MapObjectType {
    MAP_OBJECT_NONE,
    MAP_OBJECT_DELETED,
    MAP_OBJECT_MONSTER,
    MAP_OBJECT_NPC,
    MAP_OBJECT_REACTOR,
    MAP_OBJECT_DROP,
    MAP_OBJECT_DROPPING
};

struct MapObject {
    enum MapObjectType type;
    uint32_t oid;
    size_t index;
    size_t index2; // Currently used for the index of the drop in a drop batch while 'index' is used for the drop batch index itself
};

struct ObjectList {
    size_t objectCapacity;
    size_t objectCount;
    size_t freeStackTop;
    struct MapObject *objects;
    uint32_t *freeStack;
};

static int object_list_init(struct ObjectList *list);
static void object_list_destroy(struct ObjectList *list);
static uint32_t object_list_allocate(struct ObjectList *list);
static void object_list_free(struct ObjectList *list, uint32_t oid);
static struct MapObject *object_list_get(struct ObjectList *list, uint32_t oid);

struct ControllerHeapNode {
    size_t index;
    size_t controlleeCount;
    struct MapPlayer *controller;
};

struct ControllerHeap {
    size_t capacity;
    size_t count;
    struct ControllerHeapNode **controllers;
};

static int heap_init(struct ControllerHeap *heap);
static void heap_destroy(struct ControllerHeap *heap);
static struct ControllerHeapNode *heap_push(struct ControllerHeap *heap, size_t count, struct MapPlayer *client);
static size_t heap_inc(struct ControllerHeap *heap, size_t count);
static void heap_remove(struct ControllerHeap *heap, struct ControllerHeapNode *node);
static struct ControllerHeapNode *heap_top(struct ControllerHeap *heap);

struct DroppingBatch {
    size_t index;
    size_t count;
    size_t current;
    struct TimerHandle *timer;
    struct MapPlayer *owner;
    size_t indexInPlayer;
    uint32_t ownerId;
    uint32_t dropperOid;
    struct Drop drops[];
};

struct DropBatch {
    size_t count;
    struct Drop *drops;
    struct TimerHandle *timer;
    struct MapPlayer *owner;
    size_t indexInPlayer;
    uint32_t ownerId;
};

struct Map {
    struct Room *room;
    size_t playerCapacity;
    size_t playerCount;
    struct MapPlayer *players;
    struct TimerHandle *respawnHandle;
    const struct FootholdRTree *footholdTree;
    struct ObjectList objectList;
    size_t npcCount;
    struct Npc *npcs;
    size_t spawnerCount;
    struct Spawner *spawners;
    size_t monsterCapacity;
    size_t monsterCount;
    struct Monster *monsters;
    size_t deadCount;
    size_t *dead;
    struct ControllerHeap heap;
    size_t reactorCount;
    struct Reactor *reactors;
    struct ScriptManager *reactorManager;
    size_t droppingBatchCapacity;
    size_t droppingBatchCount;
    struct DroppingBatch **droppingBatches;
    size_t dropBatchCapacity;
    size_t dropBatchStart;
    size_t dropBatchEnd;
    struct DropBatch *dropBatches;
};

static void map_kill_monster(struct Map *map, uint32_t oid);
static bool map_calculate_drop_position(struct Map *map, struct Point *p);
static void map_destroy_reactor(struct Map *map, uint32_t oid);
static int map_drop_batch_from_map_object(struct Map *map, struct MapPlayer *player, struct MapObject *object, size_t count, struct Drop *drops);
static bool do_client_auto_pickup(struct Map *map, struct Client *client, struct Drop *drop);

static void on_next_drop(struct Room *room, struct TimerHandle *handle);
static void on_drop_time_expired(struct Room *room, struct TimerHandle *handle);
static void on_respawn(struct Room *room, struct TimerHandle *handle);
static void on_respawn_reactor(struct Room *room, struct TimerHandle *handle);

struct AnnounceItemDropContext {
    uint32_t charId;
    uint32_t dropperOid;
    struct Drop *drop;
};

static void do_announce_item_drop(struct Session *src, struct Session *dst, void *ctx_);

struct Map *map_create(struct Room *room, struct ScriptManager *reactor_manager)
{
    struct Map *map = malloc(sizeof(struct Map));
    if (map == NULL)
        return NULL;

    if (object_list_init(&map->objectList) == -1) {
        free(map);
        return NULL;
    }

    map->footholdTree = wz_get_foothold_tree_for_map(room_get_id(room));

    // The map ID must exist in this point so infos can't be NULL
    size_t life_count;
    const struct LifeInfo *infos = wz_get_life_for_map(room_get_id(room), &life_count);

    map->npcCount = 0;
    map->spawnerCount = 0;
    for (size_t i = 0; i < life_count; i++) {
        switch (infos[i].type) {
        case LIFE_TYPE_NPC:
            map->npcCount++;
        break;

        case LIFE_TYPE_MOB:
            map->spawnerCount++;
        break;
        }
    }

    map->npcs = malloc(map->npcCount * sizeof(struct Npc));
    if (map->npcs == NULL) {
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    map->spawners = malloc(map->spawnerCount * sizeof(struct Spawner));
    if (map->spawners == NULL && map->spawnerCount != 0) {
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    map->dead = malloc(map->spawnerCount * sizeof(size_t));
    if (map->dead == NULL && map->spawnerCount != 0) {
        free(map->spawners);
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    map->droppingBatches = malloc(sizeof(struct DroppingBatch *));
    if (map->droppingBatches == NULL) {
        free(map->dead);
        free(map->spawners);
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    map->dropBatches = malloc(sizeof(struct DropBatch));
    if (map->dropBatches == NULL) {
        free(map->droppingBatches);
        free(map->dead);
        free(map->spawners);
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    const struct MapReactorInfo *reactors_info = wz_get_reactors_for_map(room_get_id(room), &map->reactorCount);

    map->reactors = malloc(map->reactorCount * sizeof(struct Reactor));
    if (map->reactors == NULL && map->reactorCount != 0) {
        free(map->dropBatches);
        free(map->droppingBatches);
        free(map->dead);
        free(map->spawners);
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    map->players = malloc(sizeof(struct MapPlayer));
    if (map->players == NULL) {
        free(map->reactors);
        free(map->dropBatches);
        free(map->droppingBatches);
        free(map->dead);
        free(map->spawners);
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    if (heap_init(&map->heap) == -1) {
        free(map->players);
        free(map->reactors);
        free(map->dropBatches);
        free(map->droppingBatches);
        free(map->dead);
        free(map->spawners);
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    map->npcCount = 0;
    map->spawnerCount = 0;
    for (size_t i = 0; i < life_count; i++) {
        switch (infos[i].type) {
        case LIFE_TYPE_NPC: {
            uint32_t oid = object_list_allocate(&map->objectList);
            struct MapObject *object = object_list_get(&map->objectList, oid);
            object->type = MAP_OBJECT_NPC;
            object->index = map->npcCount;
            map->npcs[map->npcCount].oid = oid;
            map->npcs[map->npcCount].id = infos[i].id;
            map->npcs[map->npcCount].x = infos[i].spawnPoint.x;
            map->npcs[map->npcCount].y = infos[i].spawnPoint.y;
            map->npcs[map->npcCount].fh = infos[i].fh;
            map->npcs[map->npcCount].cy = infos[i].cy;
            map->npcs[map->npcCount].rx0 = infos[i].rx0;
            map->npcs[map->npcCount].rx1 = infos[i].rx1;
            map->npcs[map->npcCount].f = infos[i].f;
            map->npcCount++;
        }
        break;

        case LIFE_TYPE_MOB:
            map->spawners[map->spawnerCount].id = infos[i].id;
            map->spawners[map->spawnerCount].x = infos[i].spawnPoint.x;
            map->spawners[map->spawnerCount].y = infos[i].spawnPoint.y;
            map->spawners[map->spawnerCount].fh = infos[i].fh;
            map->spawnerCount++;
        break;
        }
    }

    map->monsters = malloc(map->spawnerCount * sizeof(struct Monster));
    if (map->monsters == NULL && map->spawnerCount != 0) {
        heap_destroy(&map->heap);
        free(map->players);
        free(map->reactors);
        free(map->dropBatches);
        free(map->droppingBatches);
        free(map->dead);
        free(map->spawners);
        free(map->npcs);
        object_list_destroy(&map->objectList);
        free(map);
        return NULL;
    }

    for (size_t i = 0; i < map->spawnerCount; i++) {
        uint32_t oid = object_list_allocate(&map->objectList);
        struct MapObject *obj = object_list_get(&map->objectList, oid);
        obj->type = MAP_OBJECT_MONSTER;
        obj->index = i;
        map->monsters[i].oid = oid;
        map->monsters[i].id = map->spawners[i].id;
        map->monsters[i].spawnerIndex = i;
        map->monsters[i].controller = NULL;
        map->monsters[i].x = map->spawners[i].x;
        map->monsters[i].y = map->spawners[i].y;
        map->monsters[i].fh = map->spawners[i].fh;
        map->monsters[i].hp = wz_get_monster_stats(map->spawners[i].id)->hp;
    }
    map->monsterCapacity = map->spawnerCount;
    map->monsterCount = map->spawnerCount;

    for (size_t i = 0; i < map->reactorCount; i++) {
        uint32_t oid = object_list_allocate(&map->objectList);
        struct MapObject *obj = object_list_get(&map->objectList, oid);
        obj->type = MAP_OBJECT_REACTOR;
        obj->index = i;
        map->reactors[i].id = reactors_info[i].id;
        map->reactors[i].oid = oid;
        map->reactors[i].x = reactors_info[i].pos.x;
        map->reactors[i].y = reactors_info[i].pos.y;
        map->reactors[i].state = 0;
        map->reactors[i].keepAlive = false;
    }

    map->deadCount = 0;

    map->droppingBatchCapacity = 1;
    map->droppingBatchCount = 0;

    map->dropBatchCapacity = 1;
    map->dropBatchStart = 0;
    map->dropBatchEnd = 0;

    map->playerCapacity = 1;
    map->playerCount = 0;

    map->room = room;

    map->reactorManager = reactor_manager;

    return map;
}

void map_destroy(struct Map *map)
{
    free(map->reactors);

    for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
        if (map->dropBatches[i].count != 0)
            free(map->dropBatches[i].drops);
    }

    free(map->dropBatches);

    for (size_t i = 0; i < map->droppingBatchCount; i++)
        free(map->droppingBatches[i]);

    free(map->droppingBatches);
    object_list_destroy(&map->objectList);
    heap_destroy(&map->heap);
    //for (size_t i = 0; i < map->dropBatchCount; i++) {
    //}
    free(map->dead);
    free(map->spawners);
    free(map->monsters);
    free(map->npcs);
    free(map->players);
    free(map);
}

uint32_t map_get_id(struct Map *map)
{
    return room_get_id(map->room);
}

int map_join(struct Map *map, struct Client *client, struct MapHandleContainer *player)
{
    struct Session *session = client_get_session(client);
    if (map->playerCount == map->playerCapacity) {
        void *temp = realloc(map->players, (map->playerCapacity * 2) * sizeof(struct MapPlayer));
        if (temp == NULL)
            return -1;

        map->players = temp;
        for (size_t i = 0; i < map->playerCount; i++) {
            map->players[i].container->player = &map->players[i];
            map->players[i].node->controller = &map->players[i];
            for (size_t j = 0; j < map->players[i].monsterCount; j++)
                map->players[i].monsters[j]->controller = &map->players[i];
        }

        map->playerCapacity *= 2;
    }

    player->player = &map->players[map->playerCount];

    if (map->heap.count == 0)
        map->respawnHandle = room_add_timer(map->room, 10 * 1000, on_respawn, NULL, false);

    player->player->monsters = malloc(map->spawnerCount * sizeof(struct Monster *));
    if (player->player->monsters == NULL) {
        if (map->heap.count == 0)
            room_stop_timer(map->respawnHandle);
        return -1;
    }

    player->player->monsterCount = 0;
    if (map->heap.count == 0) {
        for (size_t i = 0; i < map->monsterCount; i++) {
            if (map->monsters[i].hp > 0) {
                player->player->monsters[player->player->monsterCount] = &map->monsters[i];
                map->monsters[i].controller = player->player;
                map->monsters[i].indexInController = player->player->monsterCount;
                player->player->monsterCount++;
            }
        }
    }

    player->player->node = heap_push(&map->heap, player->player->monsterCount, player->player);
    if (player->player->node == NULL) {
        free(player->player->monsters);
        if (map->heap.count == 0)
            room_stop_timer(map->respawnHandle);
        return -1;
    }

    player->player->client = client;

    for (size_t i = 0; i < map->monsterCount; i++) {
        struct Monster *monster = &map->monsters[i];
        uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
        spawn_monster_packet(monster->oid, monster->id, monster->x, monster->y, monster->fh, false, packet);
        session_write(session, SPAWN_MONSTER_PACKET_LENGTH, packet);
    }

    for (size_t i = 0; i < player->player->monsterCount; i++) {
        struct Monster *monster = player->player->monsters[i];
        uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
        spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
        session_write(session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
    }

    const struct MapReactorInfo *reactors_info = wz_get_reactors_for_map(room_get_id(map->room), NULL);
    for (size_t i = 0; i < map->reactorCount; i++) {
        const struct ReactorInfo *info = wz_get_reactor_info(reactors_info[i].id);
        if (info->states[map->reactors[i].state].eventCount != 0) {
            uint8_t packet[SPAWN_REACTOR_PACKET_LENGTH];
            spawn_reactor_packet(map->reactors[i].oid, reactors_info[i].id, reactors_info[i].pos.x, reactors_info[i].pos.y, map->reactors[i].state, packet);
            session_write(session, SPAWN_REACTOR_PACKET_LENGTH, packet);
        }
    }

    size_t drop_count = 0;
    for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
        if (map->dropBatches[i].ownerId == client_get_character(client)->id) {
            map->dropBatches[i].owner = player->player;
            drop_count++;
        }

        for (size_t j = 0; j < map->dropBatches[i].count; j++) {
            struct Drop *drop = &map->dropBatches[i].drops[j];
            client_announce_spawn_drop(client, 0, 0, false, drop);
        }
    }

    player->player->dropCapacity = drop_count != 0 ? drop_count : 1;
    player->player->drops = malloc(player->player->dropCapacity * sizeof(struct DropBatch *));
    player->player->dropCount = 0;
    for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
        if (map->dropBatches[i].ownerId == client_get_character(client)->id) {
            player->player->drops[player->player->dropCount] = &map->dropBatches[i];
            map->dropBatches[i].indexInPlayer = player->player->dropCount;
            player->player->dropCount++;
        }
    }

    drop_count = 0;
    for (size_t i = 0; i < map->droppingBatchCount; i++) {
        if (map->droppingBatches[i]->ownerId == client_get_character(client)->id) {
            map->droppingBatches[i]->owner = player->player;
            map->droppingBatches[i]->indexInPlayer = player->player->droppingCount;
            drop_count++;
        }

        for (size_t j = 0; j < map->droppingBatches[i]->current; j++) {
            struct Drop *drop = &map->droppingBatches[i]->drops[j];
            client_announce_spawn_drop(client, 0, 0, false, drop);
        }
    }

    player->player->droppingCapacity = drop_count != 0 ? drop_count : 1;
    player->player->droppings = malloc(player->player->droppingCapacity * sizeof(struct DroppingBatch *));
    player->player->droppingCount = 0;
    for (size_t i = 0; i < map->droppingBatchCount; i++) {
        if (map->droppingBatches[i]->owner == player->player) {
            player->player->droppings[player->player->droppingCount] = map->droppingBatches[i];
            map->droppingBatches[i]->indexInPlayer = player->player->droppingCount;
            player->player->droppingCount++;
        }
    }

    player->player->container = player;
    player->player->script = NULL;
    map->playerCount++;

    return 0;
}

void map_leave(struct Map *map, struct MapPlayer *player)
{
    if (player != NULL) {
        heap_remove(&map->heap, player->node);
        struct ControllerHeapNode *next = heap_top(&map->heap);
        if (next != NULL) {
            for (size_t i = 0; i < player->monsterCount; i++) {
                player->monsters[i]->controller = next->controller;
                player->monsters[i]->indexInController = next->controller->monsterCount;
                next->controller->monsters[next->controller->monsterCount] = player->monsters[i];
                next->controller->monsterCount++;
            }
            free(player->monsters);
            for (size_t i = 0; i < next->controller->monsterCount; i++) {
                struct Monster *monster = next->controller->monsters[i];
                uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
                spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
                session_write(client_get_session(next->controller->client), SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }

            for (size_t i = 0; i < player->dropCount; i++)
                player->drops[i]->owner = NULL;

            free(player->drops);

            for (size_t i = 0; i < player->droppingCount; i++)
                player->droppings[i]->owner = NULL;

            free(player->droppings);

            if (player - map->players != map->playerCount - 1) {
                map->players[player - map->players] = map->players[map->playerCount - 1];
                map->players[player - map->players].container->player = &map->players[player - map->players];
                map->players[player - map->players].node->controller = &map->players[player - map->players];
                for (size_t i = 0; i < map->players[player - map->players].monsterCount; i++)
                    map->players[player - map->players].monsters[i]->controller = &map->players[player - map->players];
            }
        } else {
            for (size_t i = 0; i < player->monsterCount; i++)
                player->monsters[i]->controller = NULL;
            free(player->monsters);

            for (size_t i = 0; i < player->dropCount; i++)
                player->drops[i]->owner = NULL;

            free(player->drops);

            for (size_t i = 0; i < player->droppingCount; i++)
                player->droppings[i]->owner = NULL;

            free(player->droppings);
            room_stop_timer(map->respawnHandle);
        }

        map->playerCount--;
    }
}

void map_for_each_npc(struct Map *map, void (*f)(struct Npc *, void *), void *ctx)
{
    for (size_t i = 0; i < map->npcCount; i++)
        f(map->npcs + i, ctx);
}

void map_for_each_monster(struct Map *map, void (*f)(struct Monster *, void *), void *ctx)
{
    for (size_t i = 0; i < map->spawnerCount; i++)
        f(&map->monsters[i], ctx);
}

void map_for_each_drop(struct Map *map, void (*f)(struct Drop *, void *), void *ctx)
{
    for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
        for (size_t j = 0; j < map->dropBatches[i].count; j++)
            f(&map->dropBatches[i].drops[j], ctx);
    }

    for (size_t i = 0; i < map->droppingBatchCount; i++) {
        for (size_t j = 0; j < map->droppingBatches[i]->current; j++) {
            f(&map->droppingBatches[i]->drops[j], ctx);
        }
    }
}

bool map_monster_is_alive(struct Map *map, uint32_t id, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_MONSTER)
        return false;

    struct Monster *monster = &map->monsters[object->index];
    if (monster->id != id)
        return false;

    return monster->hp > 0;
}

uint32_t map_damage_monster_by(struct Map *map, struct MapPlayer *player, uint32_t char_id, uint32_t oid, size_t hit_count, int32_t *damage)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_MONSTER)
        return -1;

    struct Monster *monster = &map->monsters[object->index];

    if (monster->hp > 0) {
        if (monster->controller != player) {
            // Switch the control of the monster
            struct MapObject *object = object_list_get(&map->objectList, oid);
            struct Monster *monster = &map->monsters[object->index];
            struct MapPlayer *old = monster->controller;
            old->monsterCount--;
            old->monsters[monster->indexInController] = old->monsters[old->monsterCount];
            old->monsters[monster->indexInController]->indexInController = monster->indexInController;
            monster->controller = player;
            monster->indexInController = player->monsterCount;
            player->monsters[player->monsterCount] = monster;
            player->monsterCount++;

            {
                uint8_t packet[REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH];
                remove_monster_controller_packet(oid, packet);
                session_write(client_get_session(old->client), REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }

            {
                uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
                spawn_monster_controller_packet(oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
                session_write(client_get_session(player->client), SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }
        }

        for (size_t i = 0; i < hit_count && monster->hp > 0; i++)
            monster->hp -= damage[i];

        if (monster->hp < 0)
            monster->hp = 0;

        {
            uint8_t packet[MONSTER_HP_PACKET_LENGTH];
            monster_hp_packet(monster->oid, monster->hp * 100 / wz_get_monster_stats(monster->id)->hp, packet);
            session_write(client_get_session(player->client), MONSTER_HP_PACKET_LENGTH, packet);
        }

        if (monster->hp == 0) {
            const struct MonsterDropInfo *info = drop_info_find(monster->id);

            // Remove the controller from the monster
            if (monster->controller != NULL) {
                monster->controller->monsters[monster->indexInController] = monster->controller->monsters[monster->controller->monsterCount - 1];
                monster->controller->monsters[monster->indexInController]->indexInController = monster->indexInController;
                monster->controller->monsterCount--;
                monster->controller = NULL;
            }

            if (info != NULL) {
                struct DropInfo drops_copy[info->count];
                for (size_t i = 0; i < info->count; i++)
                    drops_copy[i] = info->drops[i];

                size_t max_drops = 0;
                for (size_t i = 0; i < info->count; i++) {
                    drops_copy[i].chance *= 16;
                    max_drops += drops_copy[i].chance / 1000000 + 1;
                }
                struct Drop drops[max_drops];
                size_t drop_count = 0;
                for (size_t i = 0; i < info->count; i++) {
                    if (drops_copy[i].chance > 1000000) {
                        enum DropType type = drops_copy[i].itemId == 0 ? DROP_TYPE_MESO : (drops_copy[i].itemId / 1000000 == 1 ? DROP_TYPE_EQUIP : DROP_TYPE_ITEM);
                        switch (type) {
                        case DROP_TYPE_MESO:
                            drops[drop_count].type = DROP_TYPE_MESO;
                            drops[drop_count].meso = rand() % (drops_copy[i].max - drops_copy[i].min + 1) + drops_copy[i].min;
                            drop_count++;
                        break;

                        case DROP_TYPE_ITEM:
                            for (size_t j = 0; j < drops_copy[i].chance / 1000000; j++) {
                                drops[drop_count].type = DROP_TYPE_ITEM;
                                drops[drop_count].qid = drops_copy[i].isQuest ? drops_copy[i].questId : 0;
                                drops[drop_count].item.item.id = 0;
                                drops[drop_count].item.item.itemId = drops_copy[i].itemId;
                                drops[drop_count].item.item.ownerLength = 0;
                                drops[drop_count].item.item.flags = 0;
                                drops[drop_count].item.item.expiration = -1;
                                drops[drop_count].item.item.giftFromLength = 0;
                                drops[drop_count].item.quantity = rand() % (drops_copy[i].max - drops_copy[i].min + 1) + drops_copy[i].min;
                                drop_count++;
                            }

                            drops_copy[i].chance %= 1000000;
                        break;

                        case DROP_TYPE_EQUIP:
                            for (size_t j = 0; j < drops_copy[i].chance / 1000000; j++) {
                                drops[drop_count].type = DROP_TYPE_EQUIP;
                                drops[drop_count].equip = equipment_from_info(wz_get_equip_info(drops_copy[i].itemId));
                                drop_count++;
                            }

                            drops_copy[i].chance %= 1000000;
                        break;
                        }

                    }

                    if (rand() % 1000000 < drops_copy[i].chance) {
                        drops[drop_count].type = drops_copy[i].itemId == 0 ? DROP_TYPE_MESO : (drops_copy[i].itemId / 1000000 == 1 ? DROP_TYPE_EQUIP : DROP_TYPE_ITEM);
                        switch (drops[drop_count].type) {
                        case DROP_TYPE_MESO:
                            drops[drop_count].meso = rand() % (drops_copy[i].max - drops_copy[i].min + 1) + drops_copy[i].min;
                        break;

                        case DROP_TYPE_ITEM:
                            drops[drop_count].qid = drops_copy[i].isQuest ? drops_copy[i].questId : 0;
                            drops[drop_count].item.item.id = 0;
                            drops[drop_count].item.item.itemId = drops_copy[i].itemId;
                            drops[drop_count].item.item.ownerLength = 0;
                            drops[drop_count].item.item.flags = 0;
                            drops[drop_count].item.item.expiration = -1;
                            drops[drop_count].item.item.giftFromLength = 0;
                            drops[drop_count].item.quantity = rand() % (drops_copy[i].max - drops_copy[i].min + 1) + drops_copy[i].min;
                        break;

                        case DROP_TYPE_EQUIP:
                            drops[drop_count].equip = equipment_from_info(wz_get_equip_info(drops_copy[i].itemId));
                        break;
                        }
                        drop_count++;
                    }
                }

                map_drop_batch_from_map_object(map, player, object, drop_count, drops);

                if (drop_count == 1)
                    map_kill_monster(map, monster->oid);
            } else {
                map_kill_monster(map, monster->oid);
            }

            return monster->id;
        }
    }

    return -1;
}

struct ClientResult map_hit_reactor(struct Map *map, struct MapPlayer *player, uint32_t oid, uint8_t stance)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_REACTOR)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    struct Reactor *reactor = &map->reactors[object->index];
    if (wz_get_reactor_info(reactor->id)->states[reactor->state].eventCount == 0)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    bool found = false;
    for (size_t i = 0; i < wz_get_reactor_info(reactor->id)->states[reactor->state].eventCount; i++) {
        if (wz_get_reactor_info(reactor->id)->states[reactor->state].events[i].type == REACTOR_EVENT_TYPE_HIT) {
            reactor->state = wz_get_reactor_info(reactor->id)->states[reactor->state].events[i].next;
            found = true;
            break;
        }
    }

    if (!found)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    if (wz_get_reactor_info(reactor->id)->states[reactor->state].eventCount == 0) {
        // Reactor broken
        char script_name[37];
        strcpy(script_name, wz_get_reactor_info(reactor->id)->action);
        strcat(script_name, ".lua");
        player->script = script_manager_alloc(map->reactorManager, script_name, 0);
        if (player->script == NULL) {
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
        }

        player->rm = reactor_manager_create(map, player->client, object->index, oid);

        struct ScriptResult res = script_manager_run(player->script, SCRIPT_REACTOR_MANAGER_TYPE, player->rm);

        switch (res.result) {
        case SCRIPT_RESULT_VALUE_KICK:
            reactor_manager_destroy(player->rm);
            script_manager_free(player->script);
            player->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        break;

        case SCRIPT_RESULT_VALUE_FAILURE:
            reactor_manager_destroy(player->rm);
            script_manager_free(player->script);
            player->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
        break;

        case SCRIPT_RESULT_VALUE_SUCCESS:
            if (!reactor->keepAlive)
                map_destroy_reactor(map, reactor->oid);

            reactor_manager_destroy(player->rm);
            script_manager_free(player->script);
            player->script = NULL;
        /* FALLTHROUGH */
        case SCRIPT_RESULT_VALUE_NEXT:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        break;

        case SCRIPT_RESULT_VALUE_WARP:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_WARP, .map = res.value.i, .portal = res.value2.i };
        break;
        }
    } else {
        // Reactor got hit
        uint8_t packet[CHANGE_REACTOR_STATE_PACKET_LENGTH];
        change_reactor_state_packet(reactor->oid, reactor->state, reactor->x, reactor->y, stance, packet);
        room_broadcast(map->room, CHANGE_REACTOR_STATE_PACKET_LENGTH, packet);
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult map_cont_script(struct Map *map, struct MapPlayer *player)
{
    script_manager_run(player->script, SCRIPT_REACTOR_MANAGER_TYPE, player->rm);
    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

const struct Npc *map_get_npc(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_NPC)
        return NULL;
    return &map->npcs[object->index];
}

bool map_move_monster(struct Map *map, struct MapPlayer *controller, uint8_t activity, uint32_t oid, int16_t x, int16_t y, uint16_t fh, uint8_t stance, size_t len, uint8_t *raw_data)
{
    struct MapObject *obj = object_list_get(&map->objectList, oid);
    if (obj == NULL || obj->type != MAP_OBJECT_MONSTER)
        return false;

    struct Monster *monster = &map->monsters[obj->index];
    if (monster->hp <= 0 || monster->controller != controller)
        return false;

    monster->x = x;
    monster->y = y;
    monster->fh = fh;
    monster->stance = stance;

    {
        uint8_t packet[MOVE_MOB_PACKET_MAX_LENGTH];
        size_t packet_len = move_monster_packet(oid, activity, len, raw_data, packet);
        session_broadcast_to_room(client_get_session(monster->controller->client), packet_len, packet);
    }

    return true;
}

int map_drop_batch_from_reactor(struct Map *map, struct MapPlayer *player, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_REACTOR)
        return -1;

    const struct MapReactorInfo *reactor = &wz_get_reactors_for_map(map_get_id(map), NULL)[object->index];
    const struct MonsterDropInfo *info = reactor_drop_info_find(reactor->id);
    struct Drop drops[info->count];

    size_t drop_count = 0;
    for (size_t i = 0; i < info->count; i++) {
        if (rand() % 1000000 < info->drops[i].chance * 16) {
            drops[drop_count].type = info->drops[i].itemId == 0 ? DROP_TYPE_MESO : (info->drops[i].itemId / 1000000 == 1 ? DROP_TYPE_EQUIP : DROP_TYPE_ITEM);
            switch (drops[drop_count].type) {
            case DROP_TYPE_MESO:
                drops[drop_count].meso = rand() % (info->drops[i].max - info->drops[i].min + 1) + info->drops[i].min;
            break;

            case DROP_TYPE_ITEM:
                drops[drop_count].qid = info->drops[i].isQuest ? info->drops[i].questId : 0;
                drops[drop_count].item.item.id = 0;
                drops[drop_count].item.item.itemId = info->drops[i].itemId;
                drops[drop_count].item.item.ownerLength = 0;
                drops[drop_count].item.item.flags = 0;
                drops[drop_count].item.item.expiration = -1;
                drops[drop_count].item.item.giftFromLength = 0;
                drops[drop_count].item.quantity = rand() % (info->drops[i].max - info->drops[i].min + 1) + info->drops[i].min;
            break;

            case DROP_TYPE_EQUIP:
                drops[drop_count].equip = equipment_from_info(wz_get_equip_info(info->drops[i].itemId));
            break;
            }
            drop_count++;
        }
    }

    return map_drop_batch_from_map_object(map, player, object, drop_count, drops);
}

static int map_drop_batch_from_map_object(struct Map *map, struct MapPlayer *player, struct MapObject *object, size_t count, struct Drop *drops)
{
    struct Point pos;
    if (object->type == MAP_OBJECT_MONSTER) {
        pos.x = map->monsters[object->index].x;
        pos.y = map->monsters[object->index].y;
    } else if (object->type == MAP_OBJECT_REACTOR) {
        pos = wz_get_reactors_for_map(room_get_id(map->room), NULL)[object->index].pos;
    } else if (object->type == MAP_OBJECT_DROP) {
        pos.x = map->dropBatches[object->index].drops[object->index2].x;
        pos.y = map->dropBatches[object->index].drops[object->index2].y;
    } else if (object->type == MAP_OBJECT_DROPPING) {
        pos.x = map->droppingBatches[object->index]->drops[object->index2].x;
        pos.y = map->droppingBatches[object->index]->drops[object->index2].y;
    } else if (object->type == MAP_OBJECT_NPC) {
        pos.x = map->npcs[object->index].x;
        pos.y = map->npcs[object->index].y;
    }

    // We need to make a copy because object_list_allocate() can invalidate `object`
    struct MapObject object_copy = *object;

    for (size_t i = 0; i < count; i++) {
        drops[i].x = pos.x + (i - count / 2) * 25;
        drops[i].y = pos.y - 85;
        struct Point pos = { drops[i].x, drops[i].y };
        map_calculate_drop_position(map, &pos);
        drops[i].x = pos.x;
        drops[i].y = pos.y;
    }

    if (count > 1) {
        if (map->droppingBatchCount == map->droppingBatchCapacity) {
            void *temp = realloc(map->droppingBatches, (map->droppingBatchCapacity * 2) * sizeof(struct DroppingBatch *));
            if (temp == NULL)
                return -1;

            map->droppingBatches = temp;
            map->droppingBatchCapacity *= 2;
        }

        if (player->droppingCount == player->droppingCapacity) {
            void *temp = realloc(player->droppings, (player->droppingCapacity * 2) * sizeof(struct DroppingBatch *));
            if (temp == NULL)
                return -1;

            player->droppings = temp;
            player->droppingCapacity *= 2;
        }

        struct DroppingBatch *batch = malloc(sizeof(struct DroppingBatch) + count * sizeof(struct Drop));
        if (batch == NULL)
            return -1;

        batch->timer = room_add_timer(map->room, 300, on_next_drop, batch, true);

        // Drop the first one immediatly
        // Also can't be player drop as they come in 1's
        struct Drop *drop = &drops[0];
        drop->oid = object_list_allocate(&map->objectList);
        struct MapObject *drop_object = object_list_get(&map->objectList, drop->oid);
        drop_object->type = MAP_OBJECT_DROPPING;
        drop_object->index = map->droppingBatchCount;
        drop_object->index2 = 0;

        struct AnnounceItemDropContext ctx = {
            .charId = client_get_character(player->client)->id,
            .dropperOid = object_copy.oid,
            .drop = drop
        };
        room_foreach(map->room, do_announce_item_drop, &ctx);

        for (size_t i = 0; i < count; i++)
            batch->drops[i] = drops[i];

        map->droppingBatches[map->droppingBatchCount] = batch;
        batch->index = map->droppingBatchCount;
        batch->count = count;
        batch->current = 1;
        batch->owner = player;
        batch->indexInPlayer = player->droppingCount;
        batch->ownerId = client_get_character(player->client)->id;
        player->droppings[player->droppingCount] = batch;
        player->droppingCount++;
        batch->dropperOid = object_copy.oid;
        map->droppingBatchCount++;

        // If the dropper is a reactor then it was triggered
        // during the reactor's script which should destroy the
        // reactor after the script is finished.
        // This prevents this from happening until all drops have been dropped
        if (object_copy.type == MAP_OBJECT_REACTOR)
            map->reactors[object_copy.index].keepAlive = true;

        if (client_is_auto_pickup_enabled(player->client))
            do_client_auto_pickup(map, player->client, drop);
    } else if (count == 1) {
        if (map->dropBatchEnd == map->dropBatchCapacity) {
            void *temp = realloc(map->dropBatches, (map->dropBatchCapacity * 2) * sizeof(struct DropBatch));
            if (temp == NULL)
                return -1; // TODO: Delete the drops

            map->dropBatches = temp;
            map->dropBatchCapacity *= 2;
            for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
                struct DropBatch *batch = &map->dropBatches[i];
                if (batch->owner != NULL)
                    batch->owner->drops[batch->indexInPlayer] = batch;
            }
        }

        if (player->dropCount == player->dropCapacity) {
            void *temp = realloc(player->drops, (player->dropCapacity * 2) * sizeof(struct DropBatch *));
            if (temp == NULL)
                return -1; // TODO: Delete the drops

            player->drops = temp;
            player->dropCapacity *= 2;
        }

        struct DropBatch *batch = &map->dropBatches[map->dropBatchEnd];
        batch->timer = room_add_timer(map->room, 15 * 1000, on_drop_time_expired, NULL, true);

        batch->drops = malloc(sizeof(struct Drop));
        *batch->drops = *drops;
        struct Drop *drop = &batch->drops[0];
        drop->oid = object_list_allocate(&map->objectList);
        struct MapObject *object = object_list_get(&map->objectList, drop->oid);
        object->type = MAP_OBJECT_DROP;
        object->index = map->dropBatchEnd;
        object->index2 = 0;
        batch->count = 1;
        batch->owner = player;
        batch->indexInPlayer = player->dropCount;
        batch->ownerId = client_get_character(player->client)->id;
        player->drops[player->dropCount] = batch;
        player->dropCount++;
        map->dropBatchEnd++;

        switch (drop->type) {
        case DROP_TYPE_MESO: {
            uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
            drop_meso_from_object_packet(drop->oid, drop->meso, batch->ownerId, drop->x, drop->y, drop->x, drop->y, object_copy.oid, false, packet);
            room_broadcast(map->room, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
        }
            break;

        case DROP_TYPE_ITEM: {
            struct AnnounceItemDropContext ctx = {
                .charId = batch->ownerId,
                .dropperOid = object_copy.oid,
                .drop = drop
            };
            room_foreach(map->room, do_announce_item_drop, &ctx);
        }
        break;

        case DROP_TYPE_EQUIP: {
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, batch->ownerId, drop->x, drop->y, drop->x, drop->y, object_copy.oid, false, packet);
            room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        }
        break;
        }

        if (client_is_auto_pickup_enabled(player->client)) {
            // TODO: Check if this client is allowed to pick up the drop
            do_client_auto_pickup(map, player->client, drop);
        }
    }

    return 0;
}

void map_pick_up_all(struct Map *map, struct MapPlayer *player)
{
    // This whole thing is a very hacky way to pick up all the player's items on the map.
    // Since map_remove_drop() also changes player->dropCount and the current batch->count
    // and moves `DropBatch`es and `Drop`s around in memory,
    // we have to be extra careful with the iterators i and j.
    for (size_t i = 0; i < player->dropCount; i++) {
        struct DropBatch *batch = player->drops[i];
        // We need to cache this `count` as `batch` can be invalidated during a call to map_remove_drop()
        size_t count = batch->count;
        for (size_t j = 0; j < count; j++) {
            struct Drop *drop = &batch->drops[j];
            if (batch->count == 1)
                i--;

            if (do_client_auto_pickup(map, player->client, drop)) {
                count--;
                j--;
            } else if (batch->count == 1) {
                i++;
            }
        }
    }

    for (size_t i = 0; i < player->droppingCount; i++) {
        struct DroppingBatch *batch = player->droppings[i];
        // We need to cache this `count` as `batch` can be invalidated during a call to map_remove_drop()
        size_t current = batch->current;
        for (size_t j = 0; j < current; j++) {
            struct Drop *drop = &batch->drops[j];
            if (do_client_auto_pickup(map, player->client, drop)) {
                current--;
                j--;
            }
        }
    }
}

void map_add_player_drop(struct Map *map, struct MapPlayer *player, struct Drop *drop)
{
    if (map->dropBatchEnd == map->dropBatchCapacity) {
        void *temp = realloc(map->dropBatches, (map->dropBatchCapacity * 2) * sizeof(struct DropBatch));
        if (temp == NULL)
            return; // TODO: Delete the drops

        map->dropBatches = temp;
        map->dropBatchCapacity *= 2;
        for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
            struct DropBatch *batch = &map->dropBatches[i];
            if (batch->owner != NULL)
                batch->owner->drops[batch->indexInPlayer] = batch;
        }
    }

    if (player->dropCount == player->dropCapacity) {
        void *temp = realloc(player->drops, (player->dropCapacity * 2) * sizeof(struct DropBatch *));
        if (temp == NULL)
            return;

        player->drops = temp;
        player->dropCapacity *= 2;
    }

    struct DropBatch *batch = &map->dropBatches[map->dropBatchEnd];
    batch->timer = room_add_timer(map->room, 15 * 1000, on_drop_time_expired, NULL, true);

    batch->drops = malloc(sizeof(struct Drop));
    *batch->drops = *drop;
    drop = &batch->drops[0];
    drop->oid = object_list_allocate(&map->objectList);
    struct MapObject *object = object_list_get(&map->objectList, drop->oid);
    object->type = MAP_OBJECT_DROP;
    object->index = map->dropBatchEnd;
    object->index2 = 0;
    batch->count = 1;
    batch->owner = player;
    batch->ownerId = client_get_character(player->client)->id;
    batch->indexInPlayer = player->dropCount;
    player->drops[player->dropCount] = batch;
    player->dropCount++;
    map->dropBatchEnd++;

    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
        drop_meso_from_object_packet(drop->oid, drop->meso, batch->ownerId, drop->x, drop->y, drop->x, drop->y, batch->ownerId, true, packet);
        room_broadcast(map->room, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_ITEM: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->item.item.itemId, batch->ownerId, drop->x, drop->y, drop->x, drop->y, batch->ownerId, true, packet);
        room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_EQUIP: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, batch->ownerId, drop->x, drop->y, drop->x, drop->y, batch->ownerId, true, packet);
        room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;
    }
}

const struct Drop *map_get_drop(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || (object->type != MAP_OBJECT_DROPPING && object->type != MAP_OBJECT_DROP))
        return NULL;

    return object->type == MAP_OBJECT_DROPPING ?
        &map->droppingBatches[object->index]->drops[object->index2] :
        &map->dropBatches[object->index].drops[object->index2];
}

void map_remove_drop(struct Map *map, uint32_t char_id, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);

    if (object->type == MAP_OBJECT_DROP) {
        size_t batch_index = object->index;
        size_t drop_index = object->index2;
        struct DropBatch *batch = &map->dropBatches[batch_index];
        object_list_free(&map->objectList, oid);

        batch->drops[drop_index] = batch->drops[batch->count - 1];
        if (drop_index != batch->count - 1) {
            struct MapObject *object = object_list_get(&map->objectList, batch->drops[drop_index].oid);
            assert(object->type == MAP_OBJECT_DROP);
            object->index2 = drop_index;
        }

        batch->count--;
        if (batch->count == 0) {
            free(batch->drops);
            if (batch->owner != NULL) {
                batch->owner->drops[batch->indexInPlayer] = batch->owner->drops[batch->owner->dropCount - 1];
                batch->owner->drops[batch->indexInPlayer]->indexInPlayer = batch->indexInPlayer;
                batch->owner->dropCount--;
                if (batch->owner->dropCount < batch->owner->dropCapacity / 4) {
                    void *temp = realloc(batch->owner->drops, (batch->owner->dropCapacity / 2) * sizeof(struct DropBatch *));
                    if (temp == NULL)
                        ; // TODO

                    batch->owner->drops = temp;
                    batch->owner->dropCapacity /= 2;
                }
            }
            room_stop_timer(batch->timer);
        }

        while (map->dropBatchStart < map->dropBatchEnd && map->dropBatches[map->dropBatchStart].count == 0) {
            map->dropBatchStart++;

            if (map->dropBatchEnd - map->dropBatchStart < map->dropBatchCapacity / 4) {
                if (map->dropBatchStart != 0) {
                    for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
                        map->dropBatches[i - map->dropBatchStart] = map->dropBatches[i];
                        if (map->dropBatches[i - map->dropBatchStart].owner != NULL) {
                            struct DropBatch *batch = &map->dropBatches[i - map->dropBatchStart];
                            batch->owner->drops[batch->indexInPlayer] = batch;
                        }
                        for (size_t j = 0; j < map->dropBatches[i - map->dropBatchStart].count; j++) {
                            struct MapObject *object =
                                object_list_get(&map->objectList, map->dropBatches[i - map->dropBatchStart].drops[j].oid);
                            assert(object->type == MAP_OBJECT_DROP);
                            object->index = i - map->dropBatchStart;
                        }
                    }

                    map->dropBatchEnd -= map->dropBatchStart;
                    map->dropBatchStart = 0;
                }

                void *temp = realloc(map->dropBatches, (map->dropBatchCapacity / 2) * sizeof(struct DropBatch));
                if (temp != NULL) {
                    map->dropBatches = temp;
                    map->dropBatchCapacity /= 2;
                    for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
                        struct DropBatch *batch = &map->dropBatches[i];
                        if (batch->owner != NULL)
                            batch->owner->drops[batch->indexInPlayer] = batch;
                    }
                }
            }
        }
    } else {
        size_t batch_index = object->index;
        size_t drop_index = object->index2;
        struct DroppingBatch *batch = map->droppingBatches[batch_index];
        object_list_free(&map->objectList, oid);

        batch->drops[drop_index] = batch->drops[batch->current - 1];
        if (drop_index != batch->current - 1) {
            struct MapObject *object = object_list_get(&map->objectList, batch->drops[drop_index].oid);
            assert(object->type == MAP_OBJECT_DROPPING);
            object->index2 = drop_index;
        }
        batch->current--;

        batch->drops[batch->current] = batch->drops[batch->count - 1];
        batch->count--;
    }

    {
        uint8_t packet[PICKUP_DROP_PACKET_LENGTH];
        pickup_drop_packet(oid, false, char_id, packet);
        room_broadcast(map->room, PICKUP_DROP_PACKET_LENGTH, packet);
    }
}

static void map_kill_monster(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    struct Monster *monster = &map->monsters[object->index];
    if (monster->spawnerIndex != -1) {
        map->dead[map->deadCount] = monster->spawnerIndex;
        map->deadCount++;
    }
    map->monsters[object->index] = map->monsters[map->monsterCount - 1];
    if (map->monsters[object->index].controller != NULL)
        map->monsters[object->index].controller->monsters[map->monsters[object->index].indexInController] = &map->monsters[object->index];
    object_list_get(&map->objectList, map->monsters[object->index].oid)->index = object->index;
    map->monsterCount--;
    object_list_free(&map->objectList, oid);
    uint8_t packet[KILL_MONSTER_PACKET_LENGTH];
    kill_monster_packet(oid, true, packet);
    room_broadcast(map->room, KILL_MONSTER_PACKET_LENGTH, packet);
}

static void map_destroy_reactor(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    struct Reactor *reactor = &map->reactors[object->index];

    uint8_t packet[DESTROY_REACTOR_PACKET_LENGTH];
    destroy_reactor_packet(oid, reactor->state, wz_get_reactors_for_map(room_get_id(map->room), NULL)[object->index].pos.x, wz_get_reactors_for_map(room_get_id(map->room), NULL)[object->index].pos.y, packet);
    room_broadcast(map->room, DESTROY_REACTOR_PACKET_LENGTH, packet);

    room_add_timer(map->room, 3000, on_respawn_reactor, reactor, true);
}

static bool map_calculate_drop_position(struct Map *map, struct Point *p)
{
    const struct Foothold *fh = foothold_tree_find_below(map->footholdTree, p);
    if (fh == NULL)
        return false;

    if (fh->p1.x == fh->p2.x)
        p->y = fh->p1.y;
    else
        p->y = (fh->p2.y - fh->p1.y) * (p->x - fh->p2.x) / (fh->p2.x - fh->p1.x) + fh->p2.y;

    return true;
}

static bool do_client_auto_pickup(struct Map *map, struct Client *client, struct Drop *drop)
{
    // TODO: Check if this client is allowed to pick up the drop
    enum InventoryGainResult result;
    uint32_t id;
    switch (drop->type) {
    case DROP_TYPE_MESO:
        client_gain_meso(client, drop->meso, true, false);
        client_commit_stats(client);
        map_remove_drop(map, client_get_character(client)->id, drop->oid);
        return true;
    break;

    case DROP_TYPE_ITEM:
        if (drop->item.item.itemId / 1000000 == 2 && wz_get_consumable_info(drop->item.item.itemId)->consumeOnPickup) {
            client_use_item_immediate(client, drop->item.item.itemId);
            map_remove_drop(map, client_get_character(client)->id, drop->oid);
            return true;
        } else if (!client_gain_inventory_item(client, &drop->item, &result)) {
            return false;
        }

        id = drop->item.item.itemId;
    break;

    case DROP_TYPE_EQUIP:
        if (!client_gain_equipment(client, &drop->equip, false, &result))
            return false;

        id = drop->equip.item.itemId;
    break;
    }

    if (result == INVENTORY_GAIN_RESULT_SUCCESS) {
        {
            uint8_t packet[ITEM_GAIN_PACKET_LENGTH];
            item_gain_packet(id, 1, packet);
            session_write(client_get_session(client), ITEM_GAIN_PACKET_LENGTH, packet);
        }

        {
            uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
            size_t len = stat_change_packet(true, 0, NULL, packet);
            session_write(client_get_session(client), len, packet);
        }

        map_remove_drop(map, client_get_character(client)->id, drop->oid);
        return true;
    } else if (result == INVENTORY_GAIN_RESULT_FULL) {
        {
            uint8_t packet[INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH];
            inventory_full_notification_packet(packet);
            session_write(client_get_session(client), INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH, packet);
        }

        {
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(0, NULL, packet);
            session_write(client_get_session(client), len, packet);
        }

        return false;
    } else if (result == INVENTORY_GAIN_RESULT_FULL) {
        {
            uint8_t packet[ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH];
            item_unavailable_notification_packet(packet);
            session_write(client_get_session(client), ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH, packet);
        }

        {
            uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
            size_t len = stat_change_packet(true, 0, NULL, packet);
            session_write(client_get_session(client), len, packet);
        }

        return false;
    }

    return false;
}

static void on_next_drop(struct Room *room, struct TimerHandle *handle)
{
    struct Map *map = room_get_context(room);
    struct DroppingBatch *batch = timer_get_data(handle);
    struct Drop *drop = &batch->drops[batch->current];
    drop->oid = object_list_allocate(&map->objectList);
    if (drop->oid == -1)
        return; // TODO

    struct MapObject *object = object_list_get(&map->objectList, drop->oid);
    object->type = MAP_OBJECT_DROPPING;
    object->index = batch->index;
    object->index2 = batch->current;

    struct AnnounceItemDropContext ctx = {
        .charId = batch->ownerId,
        .dropperOid = batch->dropperOid,
        .drop = drop
    };
    room_foreach(map->room, do_announce_item_drop, &ctx);

    batch->current++;
    if (batch->current < batch->count) {
        if (batch->owner != NULL && client_is_auto_pickup_enabled(batch->owner->client))
            do_client_auto_pickup(map, batch->owner->client, drop);
        batch->timer = room_add_timer(map->room, 300, on_next_drop, batch, true);
    } else {
        if (map->dropBatchEnd == map->dropBatchCapacity) {
            void *temp = realloc(map->dropBatches, (map->dropBatchCapacity * 2) * sizeof(struct DropBatch));
            if (temp == NULL)
                return; // TODO: Delete the drops

            map->dropBatches = temp;
            map->dropBatchCapacity *= 2;
            for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
                struct DropBatch *batch = &map->dropBatches[i];
                if (batch->owner != NULL)
                    batch->owner->drops[batch->indexInPlayer] = batch;
            }
        }

        if (batch->owner != NULL) {
            if (batch->owner->dropCount == batch->owner->dropCapacity) {
                void *temp = realloc(batch->owner->drops, (batch->owner->dropCapacity * 2) * sizeof(struct DropBatch *));
                if (temp == NULL)
                    return;

                batch->owner->drops = temp;
                batch->owner->dropCapacity *= 2;
            }
        }

        struct DropBatch *new = &map->dropBatches[map->dropBatchEnd];
        new->timer = room_add_timer(map->room, 15 * 1000, on_drop_time_expired, NULL, true);
        new->drops = malloc(batch->count * sizeof(struct Drop));

        if (batch->owner != NULL) {
            batch->owner->droppings[batch->indexInPlayer] = batch->owner->droppings[batch->owner->droppingCount - 1];
            batch->owner->droppings[batch->indexInPlayer]->indexInPlayer = batch->indexInPlayer;

            batch->owner->droppingCount--;
            if (batch->owner->droppingCount < batch->owner->droppingCapacity / 4) {
                void *temp = realloc(batch->owner->droppings, (batch->owner->droppingCapacity / 2) * sizeof(struct DroppingBatch *));
                if (temp != NULL) {
                    batch->owner->droppings = temp;
                    batch->owner->droppingCapacity /= 2;
                }
            }
        }

        map->droppingBatches[batch->index] = map->droppingBatches[map->droppingBatchCount - 1];
        map->droppingBatches[batch->index]->index = batch->index;
        for (size_t i = 0; i < map->droppingBatches[batch->index]->current; i++)
            object_list_get(&map->objectList, map->droppingBatches[batch->index]->drops[i].oid)->index = batch->index;

        map->droppingBatchCount--;
        if (map->droppingBatchCount < map->droppingBatchCapacity / 4) {
            void *temp = realloc(map->droppingBatches, (map->droppingBatchCapacity / 2) * sizeof(struct DroppingBatch *));
            if (temp != NULL) {
                map->droppingBatches = temp;
                map->droppingBatchCapacity /= 2;
            }
        }

        for (size_t i = 0; i < batch->count; i++) {
            struct MapObject *object = object_list_get(&map->objectList, batch->drops[i].oid);
            object->type = MAP_OBJECT_DROP;
            object->index = map->dropBatchEnd;
            new->drops[i] = batch->drops[i];
        }
        new->count = batch->count;
        new->owner = batch->owner;
        if (batch->owner != NULL) {
            new->indexInPlayer = batch->owner->dropCount;
            batch->owner->drops[batch->owner->dropCount] = new;
            batch->owner->dropCount++;
        }
        new->ownerId = batch->ownerId;
        map->dropBatchEnd++;

        object = object_list_get(&map->objectList, batch->dropperOid);
        if (object->type == MAP_OBJECT_MONSTER)
            map_kill_monster(map, batch->dropperOid);
        else
            map_destroy_reactor(map, batch->dropperOid);

        if (batch->owner != NULL && client_is_auto_pickup_enabled(batch->owner->client))
            do_client_auto_pickup(map, batch->owner->client, drop);
        free(batch);
    }
}

static void on_drop_time_expired(struct Room *room, struct TimerHandle *handle)
{
    struct Map *map = room_get_context(room);
    struct DropBatch *batch = &map->dropBatches[map->dropBatchStart];
    for (size_t i = 0; i < batch->count; i++) {
        struct Drop *drop = &batch->drops[i];
        uint8_t packet[REMOVE_DROP_PACKET_LENGTH];
        remove_drop_packet(drop->oid, packet);
        room_broadcast(map->room, REMOVE_DROP_PACKET_LENGTH, packet);
    }

    for (size_t i = 0; i < batch->count; i++) {
        assert(object_list_get(&map->objectList, batch->drops[i].oid)->type == MAP_OBJECT_DROP);
        object_list_free(&map->objectList, batch->drops[i].oid);
    }

    free(batch->drops);
    batch->count = 0;
    if (batch->owner != NULL) {
        batch->owner->drops[batch->indexInPlayer] = batch->owner->drops[batch->owner->dropCount - 1];
        batch->owner->drops[batch->indexInPlayer]->indexInPlayer = batch->indexInPlayer;
        batch->owner->dropCount--;
        if (batch->owner->dropCount < batch->owner->dropCapacity / 4) {
            void *temp = realloc(batch->owner->drops, (batch->owner->dropCapacity / 2) * sizeof(struct DropBatch *));
            if (temp == NULL)
                ; // TODO

            batch->owner->drops = temp;
            batch->owner->dropCapacity /= 2;
        }
    }
    while (map->dropBatchStart < map->dropBatchEnd && map->dropBatches[map->dropBatchStart].count == 0) {
        map->dropBatchStart++;

        if (map->dropBatchEnd - map->dropBatchStart < map->dropBatchCapacity / 4) {
            if (map->dropBatchStart != 0) {
                for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
                    map->dropBatches[i - map->dropBatchStart] = map->dropBatches[i];
                    for (size_t j = 0; j < map->dropBatches[i - map->dropBatchStart].count; j++) {
                        struct MapObject *object = object_list_get(&map->objectList, map->dropBatches[i - map->dropBatchStart].drops[j].oid);
                        assert(object->type == MAP_OBJECT_DROP);
                        object->index = i - map->dropBatchStart;
                    }
                }

                map->dropBatchEnd -= map->dropBatchStart;
                map->dropBatchStart = 0;
            }

            void *temp = realloc(map->dropBatches, (map->dropBatchCapacity / 2) * sizeof(struct DropBatch));
            if (temp != NULL) {
                map->dropBatches = temp;
                map->dropBatchCapacity /= 2;
                for (size_t i = map->dropBatchStart; i < map->dropBatchEnd; i++) {
                    struct DropBatch *batch = &map->dropBatches[i];
                    if (batch->owner != NULL)
                        batch->owner->drops[batch->indexInPlayer] = batch;
                }
            }
        }
    }
}

static void on_respawn(struct Room *room, struct TimerHandle *handle)
{
    struct Map *map = room_get_context(room);

    struct ControllerHeapNode *next = heap_top(&map->heap);
    for (size_t i = 0; i < map->deadCount; i++) {
        if (map->monsterCount == map->monsterCapacity) {
            struct Monster *temp = realloc(map->monsters, (map->monsterCapacity * 2) * sizeof(struct Monster));
            if (temp == NULL)
                return;

            map->monsters = temp;

            for (size_t i = 0; i < map->monsterCount; i++)
                map->monsters[i].controller->monsters[map->monsters[i].indexInController] = &map->monsters[map->monsterCount];

            map->monsterCapacity *= 2;
        }

        uint32_t oid = object_list_allocate(&map->objectList);
        struct MapObject *obj = object_list_get(&map->objectList, oid);
        obj->type = MAP_OBJECT_MONSTER;
        obj->index = map->monsterCount;
        map->monsters[map->monsterCount].oid = oid;
        map->monsters[map->monsterCount].id = map->spawners[map->dead[i]].id;
        map->monsters[map->monsterCount].spawnerIndex = map->dead[i];
        map->monsters[map->monsterCount].controller = next->controller;
        map->monsters[map->monsterCount].indexInController = next->controller->monsterCount;
        map->monsters[map->monsterCount].x = map->spawners[map->dead[i]].x;
        map->monsters[map->monsterCount].y = map->spawners[map->dead[i]].y;
        map->monsters[map->monsterCount].fh = map->spawners[map->dead[i]].fh;
        map->monsters[map->monsterCount].hp = wz_get_monster_stats(map->spawners[map->dead[i]].id)->hp;
        next->controller->monsters[next->controller->monsterCount] = &map->monsters[map->monsterCount];
        next->controller->monsterCount++;
        map->monsterCount++;
    }

    heap_inc(&map->heap, map->deadCount);

    for (size_t i = map->monsterCount - map->deadCount; i < map->monsterCount; i++) {
        uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
        spawn_monster_packet(map->monsters[i].oid, map->monsters[i].id, map->monsters[i].x, map->monsters[i].y, map->monsters[i].fh, true, packet);
        room_broadcast(map->room, SPAWN_MONSTER_PACKET_LENGTH, packet);
    }

    for (size_t i = map->monsterCount - map->deadCount; i < map->monsterCount; i++) {
        uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
        spawn_monster_controller_packet(map->monsters[i].oid, false, map->monsters[i].id, map->monsters[i].x, map->monsters[i].y, map->monsters[i].fh, true, packet);
        session_write(client_get_session(next->controller->client), SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
    }

    map->deadCount = 0;

    map->respawnHandle = room_add_timer(map->room, 10 * 1000, on_respawn, NULL, false);
}

static void on_respawn_reactor(struct Room *room, struct TimerHandle *handle)
{
    struct Reactor *reactor = timer_get_data(handle);
    reactor->state = 0;
    reactor->keepAlive = false;

    uint8_t packet[SPAWN_REACTOR_PACKET_LENGTH];
    spawn_reactor_packet(reactor->oid, reactor->id, reactor->x, reactor->y, reactor->state, packet);
    room_broadcast(room, SPAWN_REACTOR_PACKET_LENGTH, packet);
}

static void do_announce_item_drop(struct Session *n_, struct Session *dst, void *ctx_)
{
    struct AnnounceItemDropContext *ctx = ctx_;
    struct Client *client = session_get_context(dst);
    client_announce_drop(client, ctx->charId, ctx->dropperOid, false, ctx->drop);
}

static int object_list_init(struct ObjectList *list)
{
    list->objects = malloc(sizeof(struct MapObject));
    if (list->objects == NULL)
        return -1;

    list->freeStack = malloc(sizeof(uint32_t));
    if (list->freeStack == NULL) {
        free(list->objects);
        return -1;
    }

    list->objectCapacity = 1;
    list->objectCount = 0;
    list->freeStackTop = 0;
    list->objects[0].type = MAP_OBJECT_NONE;
    return 0;
}

static void object_list_destroy(struct ObjectList *list)
{
    free(list->objects);
    free(list->freeStack);
}

static uint32_t object_list_allocate(struct ObjectList *list)
{
    // Highly unlikely, but just to make sure that we don't overflow oid
    if (list->objectCount == UINT32_MAX)
        return -1;

    if (list->objectCount == list->objectCapacity) {
        struct MapObject *new = malloc((list->objectCapacity * 2) * sizeof(struct MapObject));
        if (new == NULL)
            return -1;

        void *temp = realloc(list->freeStack, (list->objectCapacity * 2) * sizeof(uint32_t));
        if (temp == NULL) {
            free(new);
            return -1;
        }

        list->freeStack = temp;

        for (size_t i = 0; i < list->objectCapacity * 2; i++)
            new[i].type = MAP_OBJECT_NONE;

        for (size_t i = 0; i < list->objectCapacity; i++) {
            size_t new_i = XXH3_64bits(&list->objects[i].oid, sizeof(uint32_t)) % (list->objectCapacity * 2);
            while (new[new_i].type != MAP_OBJECT_NONE) {
                new_i++;
                new_i %= list->objectCapacity * 2;
            }

            new[new_i] = list->objects[i];
        }

        free(list->objects);
        list->objects = new;
        list->objectCapacity *= 2;
    }

    uint32_t oid;
    if (list->freeStackTop > 0) {
        list->freeStackTop--;
        oid = list->freeStack[list->freeStackTop];
    } else {
        oid = list->objectCount;
    }

    size_t index = XXH3_64bits(&oid, sizeof(uint32_t)) % list->objectCapacity;
    while (list->objects[index].type != MAP_OBJECT_NONE && list->objects[index].type != MAP_OBJECT_DELETED) {
        index++;
        index %= list->objectCapacity;
    }

    list->objects[index].type = MAP_OBJECT_NONE;
    list->objects[index].oid = oid;

    list->objectCount++;
    return oid;
}

static void object_list_free(struct ObjectList *list, uint32_t oid)
{
    size_t index = XXH3_64bits(&oid, sizeof(uint32_t)) % list->objectCapacity;
    while (list->objects[index].type == MAP_OBJECT_DELETED || list->objects[index].oid != oid) {
        assert(list->objects[index].type != MAP_OBJECT_NONE);
        index++;
        index %= list->objectCapacity;
    }

    list->objects[index].type = MAP_OBJECT_DELETED;
    list->freeStack[list->freeStackTop] = oid;
    list->freeStackTop++;
    list->objectCount--;
    if (list->objectCount < list->objectCapacity / 4) {
        struct MapObject *temp = malloc((list->objectCapacity / 2) * sizeof(struct MapObject));
        if (temp == NULL)
            return;

        for (size_t i = 0; i < list->objectCapacity / 2; i++)
            temp[i].type = MAP_OBJECT_NONE;

        for (size_t i = 0; i < list->objectCapacity; i++) {
            if (list->objects[i].type != MAP_OBJECT_NONE && list->objects[i].type != MAP_OBJECT_DELETED) {
                size_t new_i = XXH3_64bits(&list->objects[i].oid, sizeof(uint32_t)) % (list->objectCapacity / 2);
                while (temp[new_i].type != MAP_OBJECT_NONE) {
                    new_i++;
                    new_i %= list->objectCapacity / 2;
                }

                temp[new_i] = list->objects[i];
           }
        }

        free(list->objects);
        list->objects = temp;
        list->objectCapacity /= 2;
    }
}

static struct MapObject *object_list_get(struct ObjectList *list, uint32_t oid)
{
    if (list->objectCount == 0)
        return NULL;

    size_t start = XXH3_64bits(&oid, sizeof(uint32_t)) % list->objectCapacity;
    size_t index = start;
    while (list->objects[index].type == MAP_OBJECT_DELETED || (list->objects[index].type != MAP_OBJECT_NONE && list->objects[index].oid != oid)) {
        index++;
        index %= list->objectCapacity;
        if (index == start)
            return NULL;
    }

    if (list->objects[index].type != MAP_OBJECT_NONE && list->objects[index].oid != oid)
        return NULL;

    return &list->objects[index];
}

static int heap_init(struct ControllerHeap *heap)
{
    heap->controllers = malloc(sizeof(struct ControllerHeapNode *));
    if (heap->controllers == NULL)
        return -1;

    heap->capacity = 1;
    heap->count = 0;
    return 0;
}

static void heap_destroy(struct ControllerHeap *heap)
{
    for (size_t i = 0; i < heap->count; i++)
        free(heap->controllers[i]);

    free(heap->controllers);
}

static void sift_down(struct ControllerHeap *heap, size_t i);
static void sift_up(struct ControllerHeap *heap, size_t i);

static struct ControllerHeapNode *heap_push(struct ControllerHeap *heap, size_t count, struct MapPlayer *controller)
{
    struct ControllerHeapNode *node = malloc(sizeof(struct ControllerHeapNode));
    if (node == NULL)
        return NULL;

    if (heap->count == heap->capacity) {
        void *temp = realloc(heap->controllers, (heap->capacity * 2) * sizeof(struct ControllerHeapNode *));
        if (temp == NULL) {
            free(node);
            return NULL;
        }

        heap->controllers = temp;
        heap->capacity *= 2;
    }

    node->index = heap->count;
    node->controlleeCount = count;
    node->controller = controller;

    heap->controllers[heap->count] = node;
    heap->count++;

    sift_up(heap, heap->count - 1);

    return node;
}

static size_t heap_inc(struct ControllerHeap *heap, size_t count)
{
    heap->controllers[0]->controlleeCount += count;
    sift_down(heap, 0);
    return 0;
}

static void heap_remove(struct ControllerHeap *heap, struct ControllerHeapNode *node)
{
    heap->controllers[node->index] = heap->controllers[heap->count - 1];
    heap->controllers[node->index]->index = node->index;
    heap->count--;
    if (node->index == 0 || heap->controllers[(node->index-1) / 2]->controlleeCount < heap->controllers[node->index]->controlleeCount)
        sift_down(heap, node->index);
    else
        sift_up(heap, node->index);

    free(node);
}

static struct ControllerHeapNode *heap_top(struct ControllerHeap *heap)
{
    if (heap->count == 0)
        return NULL;

    return heap->controllers[0];
}

static void swap(struct ControllerHeap *heap, size_t i, size_t j);

static void sift_down(struct ControllerHeap *heap, size_t i)
{
    while (i*2 + 1 < heap->count) {
        if (i*2 + 2 == heap->count) {
            if (heap->controllers[i]->controlleeCount > heap->controllers[i*2 + 1]->controlleeCount)
                swap(heap, i, i*2 + 1);

            break;
        }

        if (heap->controllers[i]->controlleeCount <= heap->controllers[i*2 + 1]->controlleeCount && heap->controllers[i]->controlleeCount <= heap->controllers[i*2 + 2]->controlleeCount) {
            break;
        }

        if (heap->controllers[i]->controlleeCount > heap->controllers[i*2 + 1]->controlleeCount && heap->controllers[i]->controlleeCount > heap->controllers[i*2 + 2]->controlleeCount) {
            if (heap->controllers[i*2 + 1]->controlleeCount < heap->controllers[i*2 + 2]->controlleeCount) {
                swap(heap, i, i*2 + 1);
                i = i*2 + 1;
            } else {
                swap(heap, i, i*2 + 2);
                i = i*2 + 2;
            }
        } else if (heap->controllers[i]->controlleeCount > heap->controllers[i*2 + 1]->controlleeCount) {
            swap(heap, i, i*2 + 1);
            i = i*2 + 1;
        } else {
            swap(heap, i, i*2 + 2);
            i = i*2 + 2;
        }
    }
}

static void sift_up(struct ControllerHeap *heap, size_t i)
{
    while (i > 0) {
        if (heap->controllers[(i-1) / 2]->controlleeCount > heap->controllers[i]->controlleeCount) {
            swap(heap, (i-1) / 2, i);
            i = (i-1) / 2;
        } else {
            break;
        }
    }
}

static void swap(struct ControllerHeap *heap, size_t i, size_t j)
{
    struct ControllerHeapNode *tmp = heap->controllers[i];
    heap->controllers[i] = heap->controllers[j];
    heap->controllers[i]->index = i;
    heap->controllers[j] = tmp;
    heap->controllers[j]->index = j;
}


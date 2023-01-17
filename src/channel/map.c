#include "map.h"

#include <stdio.h>
#include <stdlib.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "server.h"
#include "drops.h"
#include "../packet.h"
#include "../wz.h"

struct MapHandle {
    struct MapHandleContainer *container;
    struct ControllerHeapNode *node;
    size_t count;
    struct Monster **monsters;
    struct Session *session;
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
    struct MapHandle *controller;
    size_t indexInController;
    int16_t x;
    int16_t y;
    uint16_t fh;
    uint8_t stance;
    int32_t hp;
};

enum MapObjectType {
    MAP_OBJECT_NONE,
    MAP_OBJECT_DELETED,
    MAP_OBJECT_MONSTER,
    MAP_OBJECT_NPC,
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
    struct MapHandle *controller;
};

struct ControllerHeap {
    size_t capacity;
    size_t count;
    struct ControllerHeapNode **controllers;
};

static int heap_init(struct ControllerHeap *heap);
static void heap_destroy(struct ControllerHeap *heap);
static struct ControllerHeapNode *heap_push(struct ControllerHeap *heap, size_t count, struct MapHandle *client);
static size_t heap_inc(struct ControllerHeap *heap, size_t count);
static void heap_remove(struct ControllerHeap *heap, struct ControllerHeapNode *node);
static struct ControllerHeapNode *heap_top(struct ControllerHeap *heap);

struct DroppingBatch {
    size_t index;
    size_t count;
    size_t current;
    //struct Drop *drops;
    struct TimerHandle *timer;
    uint32_t char_id;
    uint32_t monster_oid;
    struct Drop drops[];
};

struct DropBatch {
    size_t count;
    struct Drop *drops;
    struct TimerHandle *timer;
};

struct Map {
    struct Room *room;
    size_t handleCapacity;
    size_t handleCount;
    struct MapHandle *handles;
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
    size_t droppingBatchCapacity;
    size_t droppingBatchCount;
    struct DroppingBatch **droppingBatches;
    size_t dropBatchCapacity;
    size_t dropBatchStart;
    size_t dropBatchEnd;
    struct DropBatch *dropBatches;
};

static void on_next_drop(struct Room *room, struct TimerHandle *handle);
static void on_drop_time_expired(struct Room *room, struct TimerHandle *handle);
static void on_respawn(struct Room *room, struct TimerHandle *handle);

static void notify_monster_control_on_map(uint32_t oid, struct Monster *monster, struct Session *session);

struct Map *map_create(struct Room *room)
{
    struct Map *map = malloc(sizeof(struct Map));
    if (map == NULL)
        return NULL;

    object_list_init(&map->objectList);

    map->footholdTree = wz_get_foothold_tree_for_map(room_get_id(room));

    size_t life_count;
    const struct LifeInfo *infos = wz_get_life_for_map(room_get_id(room), &life_count);
    if (infos == NULL) {
        free(map);
        return NULL;
    }

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
        free(map);
        return NULL;
    }

    map->spawners = malloc(map->spawnerCount * sizeof(struct Spawner));
    if (map->spawners == NULL) {
        free(map->npcs);
        free(map);
        return NULL;
    }

    map->dead = malloc(map->spawnerCount * sizeof(size_t));
    if (map->dead == NULL) {
        free(map->spawners);
        free(map->npcs);
        free(map);
        return NULL;
    }

    if (heap_init(&map->heap) == -1) {
        free(map->dead);
        free(map->spawners);
        free(map->npcs);
        free(map);
        return NULL;
    }

    map->deadCount = 0;

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

    map->handles = malloc(sizeof(struct MapHandle));
    map->handleCapacity = 1;
    map->handleCount = 0;

    map->droppingBatches = malloc(sizeof(struct DroppingBatch *));
    map->droppingBatchCapacity = 1;
    map->droppingBatchCount = 0;

    map->dropBatches = malloc(sizeof(struct DropBatch));
    map->dropBatchCapacity = 1;
    map->dropBatchStart = 0;
    map->dropBatchEnd = 0;

    map->room = room;

    return map;
}

void map_destroy(struct Map *map)
{
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
    free(map->handles);
    free(map);
}

int map_join(struct Map *map, struct Session *session, struct MapHandleContainer *handle)
{
    if (map->handleCount == map->handleCapacity) {
        void *temp = realloc(map->handles, (map->handleCapacity * 2) * sizeof(struct MapHandle));
        if (temp == NULL)
            return -1;

        map->handles = temp;
        for (size_t i = 0; i < map->handleCount; i++) {
            map->handles[i].container->handle = &map->handles[i];
            map->handles[i].node->controller = &map->handles[i];
            for (size_t j = 0; j < map->handles[i].count; j++)
                map->handles[i].monsters[j]->controller = &map->handles[i];
        }

        map->handleCapacity *= 2;
    }

    handle->handle = &map->handles[map->handleCount];

    if (map->heap.count == 0)
        map->respawnHandle = room_add_timer(map->room, 10 * 1000, on_respawn, NULL, false);

    handle->handle->monsters = malloc(map->spawnerCount * sizeof(struct Monster *));
    if (handle->handle->monsters == NULL) {
        room_stop_timer(map->respawnHandle);
        return -1;
    }

    handle->handle->count = 0;
    if (map->heap.count == 0) {
        for (size_t i = 0; i < map->monsterCount; i++) {
            if (map->monsters[i].hp > 0) {
                handle->handle->monsters[handle->handle->count] = &map->monsters[i];
                map->monsters[i].controller = handle->handle;
                map->monsters[i].indexInController = handle->handle->count;
                handle->handle->count++;
            }
        }
    }

    handle->handle->node = heap_push(&map->heap, handle->handle->count, handle->handle);
    if (handle->handle->node == NULL) {
        free(handle->handle->monsters);
        free(handle->handle);
        return -1;
    }

    handle->handle->session = session;

    for (size_t i = 0; i < map->monsterCount; i++) {
        struct Monster *monster = &map->monsters[i];
        uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
        spawn_monster_packet(monster->oid, monster->id, monster->x, monster->y, monster->fh, false, packet);
        session_write(session, SPAWN_MONSTER_PACKET_LENGTH, packet);
    }

    for (size_t i = 0; i < handle->handle->count; i++) {
        struct Monster *monster = handle->handle->monsters[i];
        uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
        spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
        session_write(session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
    }

    handle->handle->container = handle;
    map->handleCount++;

    return 0;
}

void map_leave(struct Map *map, struct MapHandle *handle)
{
    if (handle != NULL) {
        heap_remove(&map->heap, handle->node);
        struct ControllerHeapNode *next = heap_top(&map->heap);
        if (next != NULL) {
            for (size_t i = 0; i < handle->count; i++) {
                handle->monsters[i]->controller = next->controller;
                handle->monsters[i]->indexInController = next->controller->count;
                next->controller->monsters[next->controller->count] = handle->monsters[i];
                next->controller->count++;
            }
            free(handle->monsters);
            monster_controller_for_each(next->controller, notify_monster_control_on_map);
            if (handle - map->handles != map->handleCount - 1) {
                map->handles[handle - map->handles] = map->handles[map->handleCount - 1];
                map->handles[handle - map->handles].container->handle = &map->handles[handle - map->handles];
                map->handles[handle - map->handles].node->controller = &map->handles[handle - map->handles];
                for (size_t i = 0; i < map->handles[handle - map->handles].count; i++)
                    map->handles[handle - map->handles].monsters[i]->controller = &map->handles[handle - map->handles];
            }
        } else {
            for (size_t i = 0; i < handle->count; i++)
                handle->monsters[i]->controller = NULL;
            free(handle->monsters);
            room_stop_timer(map->respawnHandle);
        }

        map->handleCount--;
    }
}

void map_for_each_npc(struct Map *map, void (*f)(struct Npc *, void *), void *ctx)
{
    for (size_t i = 0; i < map->npcCount; i++)
        f(map->npcs + i, ctx);
}

void map_for_each_monster(struct Map *map, void (*f)(struct Monster *, void *), void *ctx)
{
    for (size_t i = 0; i < map->spawnerCount; i++) {
        f(&map->monsters[i], ctx);
    }
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

uint32_t map_damage_monster_by(struct Map *map, struct MapHandle *handle, uint32_t char_id, uint32_t oid, size_t hit_count, int32_t *damage)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    struct Monster *monster = &map->monsters[object->index];

    if (map_is_monster_alive(map, oid)) {
        if (!map_is_monster_controlled_by(map, handle, oid)) {
            struct MapHandle *old = map_switch_control(map, handle, oid);
            {
                uint8_t packet[REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH];
                remove_monster_controller_packet(oid, packet);
                session_write(monster_controller_get_session(old), REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }

            {
                uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
                spawn_monster_controller_packet(oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
                session_write(handle->session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }
        }

        for (size_t i = 0; i < hit_count && monster->hp > 0; i++)
            monster->hp -= damage[i];

        if (monster->hp < 0)
            monster->hp = 0;

        {
            uint8_t packet[MONSTER_HP_PACKET_LENGTH];
            monster_hp_packet(monster->oid, monster->hp * 100 / wz_get_monster_stats(monster->id)->hp, packet);
            session_write(handle->session, MONSTER_HP_PACKET_LENGTH, packet);
        }

        if (monster->hp == 0) {
            const struct MonsterDropInfo *info = drop_info_find(monster->id);
            map_make_monster_ethereal(map, monster->oid);
            if (info != NULL) {
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

                for (size_t i = 0; i < drop_count; i++) {
                    drops[i].pos.x = monster->x + (i - drop_count / 2) * 25;
                    drops[i].pos.y = monster->y - 85;
                    if (!map_calculate_drop_position(map, &drops[i].pos)) {
                        drops->pos.x = monster->x;
                        drops->pos.y = monster->y;
                    }
                }

                map_add_drop_batch(map, char_id, monster->oid, drop_count, drops);
            } else {
                map_kill_monster(map, monster->oid);
            }

            return monster->id;
        }
    }

    return -1;
}

struct MapHandle *map_switch_control(struct Map *map, struct MapHandle *new, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    struct Monster *monster = &map->monsters[object->index];
    struct MapHandle *old = monster->controller;
    old->count--;
    old->monsters[monster->indexInController] = old->monsters[old->count];
    old->monsters[monster->indexInController]->indexInController = monster->indexInController;
    monster->controller = new;
    monster->indexInController = new->count;
    new->monsters[new->count] = monster;
    new->count++;
    return old;
}

const struct Monster *map_get_monster(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    return &map->monsters[object->index];
}

const struct Npc *map_get_npc(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_NPC)
        return NULL;
    return &map->npcs[object->index];
}

bool map_move_monster(struct Map *map, struct MapHandle *controller, uint8_t activity, uint32_t oid, int16_t x, int16_t y, uint16_t fh, uint8_t stance, size_t len, uint8_t *raw_data)
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
        session_broadcast_to_room(monster->controller->session, packet_len, packet);
    }

    return true;
}

bool map_is_monster_alive(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_MONSTER)
        return false;

    struct Monster *monster = &map->monsters[object->index];
    return monster->hp > 0;
}

void map_make_monster_ethereal(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    struct Monster *monster = &map->monsters[object->index];
    if (monster->controller != NULL) {
        monster->controller->monsters[monster->indexInController] = monster->controller->monsters[monster->controller->count - 1];
        monster->controller->monsters[monster->indexInController]->indexInController = monster->indexInController;
        monster->controller->count--;
        monster->controller = NULL;
    }
}

void map_kill_monster(struct Map *map, uint32_t oid)
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

void map_respawn(struct Map *map)
{
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
        map->monsters[map->monsterCount].indexInController = next->controller->count;
        map->monsters[map->monsterCount].x = map->spawners[map->dead[i]].x;
        map->monsters[map->monsterCount].y = map->spawners[map->dead[i]].y;
        map->monsters[map->monsterCount].fh = map->spawners[map->dead[i]].fh;
        map->monsters[map->monsterCount].hp = wz_get_monster_stats(map->spawners[map->dead[i]].id)->hp;
        next->controller->monsters[next->controller->count] = &map->monsters[map->monsterCount];
        next->controller->count++;
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
        session_write(next->controller->session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
    }

    map->deadCount = 0;
}

bool map_is_monster_controlled_by(struct Map *map, struct MapHandle *controller, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_MONSTER || map->monsters[object->index].controller != controller)
        return false;

    return true;
}

void map_add_drop_batch(struct Map *map, uint32_t char_id, uint32_t monster_oid, size_t count, struct Drop *drops)
{
    if (count == 0) {
        return;
    } else if (count != 1) {
        if (map->droppingBatchCount == map->droppingBatchCapacity) {
            void *temp = realloc(map->droppingBatches, (map->droppingBatchCapacity * 2) * sizeof(struct DroppingBatch *));
            if (temp == NULL)
                return;

            map->droppingBatches = temp;
            map->droppingBatchCapacity *= 2;
        }

        struct DroppingBatch *batch = malloc(sizeof(struct DroppingBatch) + count * sizeof(struct Drop));
        if (batch == NULL)
            return;

        batch->timer = room_add_timer(map->room, 300, on_next_drop, batch, true);

        // Drop the first one immediatly
        struct Drop *drop = &drops[0];
        drop->oid = object_list_allocate(&map->objectList);
        struct MapObject *object = object_list_get(&map->objectList, drop->oid);
        object->type = MAP_OBJECT_DROPPING;
        object->index = map->droppingBatchCount;
        object->index2 = 0;
        switch (drop->type) {
        case DROP_TYPE_MESO: {
            uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
            drop_meso_from_object_packet(drop->oid, char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, monster_oid, packet);
            room_broadcast(map->room, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
        }

            break;
        case DROP_TYPE_ITEM: {
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->item.item.itemId, char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, monster_oid, packet);
            room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        }
        break;

        case DROP_TYPE_EQUIP: {
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, monster_oid, packet);
            room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        }
        break;
        }

        for (size_t i = 0; i < count; i++)
            batch->drops[i] = drops[i];

        map->droppingBatches[map->droppingBatchCount] = batch;
        batch->index = map->droppingBatchCount;
        batch->count = count;
        batch->current = 1;
        batch->char_id = char_id;
        batch->monster_oid = monster_oid;
        map->droppingBatchCount++;
    } else {
        if (map->dropBatchEnd == map->dropBatchCapacity) {
            void *temp = realloc(map->dropBatches, (map->dropBatchCapacity * 2) * sizeof(struct DropBatch));
            if (temp == NULL)
                return; // TODO: Delete the drops

            map->dropBatches = temp;
            map->dropBatchCapacity *= 2;
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
        map->dropBatchEnd++;

        switch (drop->type) {
        case DROP_TYPE_MESO: {
            uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
            drop_meso_from_object_packet(drop->oid, char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, monster_oid, packet);
            room_broadcast(map->room, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
        }
        break;

        case DROP_TYPE_ITEM: {
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->item.item.itemId, char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, monster_oid, packet);
            room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        }
        break;

        case DROP_TYPE_EQUIP: {
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, monster_oid, packet);
            room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        }
        break;
        }

        if (monster_oid != -1)
            map_kill_monster(map, monster_oid);
    }
}

bool map_remove_drop(struct Map *map, uint32_t char_id, uint32_t oid, struct Drop *drop)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || (object->type != MAP_OBJECT_DROPPING && object->type != MAP_OBJECT_DROP))
        return false;

    if (object->type == MAP_OBJECT_DROP) {
        size_t batch_index = object->index;
        size_t drop_index = object->index2;
        struct DropBatch *batch = &map->dropBatches[batch_index];
        object_list_free(&map->objectList, oid);

        *drop = batch->drops[drop_index];
        batch->drops[drop_index] = batch->drops[batch->count - 1];
        if (drop_index != batch->count - 1) {
            struct MapObject *object = object_list_get(&map->objectList, batch->drops[drop_index].oid);
            assert(object->type == MAP_OBJECT_DROP);
            object->index2 = drop_index;
        }

        batch->count--;
        if (batch->count == 0) {
            free(batch->drops);
            room_stop_timer(batch->timer);
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
                }
            }
        }
    } else {
        size_t batch_index = object->index;
        size_t drop_index = object->index2;
        struct DroppingBatch *batch = map->droppingBatches[batch_index];
        object_list_free(&map->objectList, oid);

        *drop = batch->drops[drop_index];
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
        pickup_drop_packet(drop->oid, false, char_id, packet);
        room_broadcast(map->room, PICKUP_DROP_PACKET_LENGTH, packet);
    }

    return true;
}

bool map_calculate_drop_position(struct Map *map, struct Point *p)
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

void monster_controller_for_each(struct MapHandle *controller, void (*f)(uint32_t, struct Monster *, struct Session *))
{
    for (size_t i = 0; i < controller->count; i++) {
        f(controller->monsters[i]->oid, controller->monsters[i], controller->session);
    }
}

struct Session *monster_controller_get_session(struct MapHandle *controller)
{
    return controller->session;
}

static void on_next_drop(struct Room *room, struct TimerHandle *handle)
{
    struct Map *map = room_get_context(room);
    struct DroppingBatch *batch = timer_get_data(handle);
    struct Drop *drop = &batch->drops[batch->current];
    drop->oid = object_list_allocate(&map->objectList);
    struct MapObject *object = object_list_get(&map->objectList, drop->oid);
    object->type = MAP_OBJECT_DROPPING;
    object->index = batch->index;
    object->index2 = batch->current;
    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
        drop_meso_from_object_packet(drop->oid, batch->char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, batch->monster_oid, packet);
        room_broadcast(map->room, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
    }

    break;
    case DROP_TYPE_ITEM: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->item.item.itemId, batch->char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, batch->monster_oid, packet);
        room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_EQUIP: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, batch->char_id, drop->pos.x, drop->pos.y, drop->pos.x, drop->pos.y, batch->monster_oid, packet);
        room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;
    }

    batch->current++;
    if (batch->current < batch->count) {
        batch->timer = room_add_timer(map->room, 300, on_next_drop, batch, true);
    } else {
        if (map->dropBatchEnd == map->dropBatchCapacity) {
            void *temp = realloc(map->dropBatches, (map->dropBatchCapacity * 2) * sizeof(struct DropBatch));
            if (temp == NULL)
                return; // TODO: Delete the drops

            map->dropBatches = temp;
            map->dropBatchCapacity *= 2;
        }

        struct DropBatch *new = &map->dropBatches[map->dropBatchEnd];
        new->timer = room_add_timer(map->room, 15 * 1000, on_drop_time_expired, NULL, true);
        new->drops = malloc(batch->count * sizeof(struct Drop));

        map->droppingBatches[batch->index] = map->droppingBatches[map->droppingBatchCount - 1];
        map->droppingBatches[batch->index]->index = batch->index;
        for (size_t i = 0; i < map->droppingBatches[batch->index]->current; i++) {
            object_list_get(&map->objectList, map->droppingBatches[batch->index]->drops[i].oid)->index = batch->index;
        }
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
        map->dropBatchEnd++;

        if (batch->monster_oid != -1)
            map_kill_monster(map, batch->monster_oid);

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
            }
        }
    }
}

static void on_respawn(struct Room *room, struct TimerHandle *handle)
{
    struct Map *map = room_get_context(room);
    map_respawn(map);

    map->respawnHandle = room_add_timer(map->room, 10 * 1000, on_respawn, NULL, false);
}

static void notify_monster_control_on_map(uint32_t oid, struct Monster *monster, struct Session *session)
{
    uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
    spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
    session_write(session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
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

    uint32_t oid;
    if (list->freeStackTop > 0) {
        list->freeStackTop--;
        oid = list->freeStack[list->freeStackTop];
    } else {
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

static struct ControllerHeapNode *heap_push(struct ControllerHeap *heap, size_t count, struct MapHandle *controller)
{
    struct ControllerHeapNode *node = malloc(sizeof(struct ControllerHeapNode));
    if (node == NULL)
        return NULL;

    if (heap->count == heap->capacity) {
        void *temp = realloc(heap->controllers, (heap->capacity) * 2 * sizeof(struct ControllerHeapNode *));
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


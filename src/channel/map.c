#include "map.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <unistd.h>
#include <sys/eventfd.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../hash-map.h"
#include "../packet.h"
#include "../wz.h"
#include "client.h"
#include "drops.h"
#include "events.h"
#include "life.h"
#include "scripting/reactor-manager.h"
#include "server.h"

enum MapObjectType {
    MAP_OBJECT_NONE,
    MAP_OBJECT_DELETED,
    MAP_OBJECT_MONSTER,
    MAP_OBJECT_NPC,
    MAP_OBJECT_REACTOR,
    MAP_OBJECT_DROP,
    MAP_OBJECT_DROPPING,
    MAP_OBJECT_BOSS,
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
    size_t freeStackCapacity;
    size_t freeStackTop;
    struct MapObject *objects;
    uint32_t *freeStack;
};

struct MapPlayer {
    struct MapHandleContainer *container;
    struct ControllerHeapNode *node;
    size_t monsterCount;
    struct MapMonster **monsters;
    // Drops that this player owns
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

struct MapMonster {
    struct Monster monster;
    size_t spawnerIndex;
    struct MapPlayer *controller;
    size_t indexInController;
};

struct Reactor {
    uint32_t oid;
    uint32_t id;
    int16_t x;
    int16_t y;
    uint8_t state;
    bool keepAlive;
};

static int object_list_init(struct ObjectList *list);
static void object_list_destroy(struct ObjectList *list);
static struct MapObject *object_list_allocate(struct ObjectList *list);
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
    bool exclusive;
};

struct Map {
    struct Room *room;
    int fd;
    uint32_t listener;
    struct ChannelServer *server;
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
    struct MapMonster *monsters;
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
    bool *occupiedSeats;
    struct Spawner bossSpawner;
    struct MapMonster boss;
};

static void on_boat_state_changed(void *ctx);
static int dock_undock_boat(struct Room *room, int fd, int status);
static int start_sailing(struct Room *room, int fd, int status);
static int end_sailing(struct Room *room, int fd, int status);

static int dock_undock_train(struct Room *room, int fd, int status);
static int start_train(struct Room *room, int fd, int status);
static int end_train(struct Room *room, int fd, int status);

static int dock_undock_genie(struct Room *room, int fd, int status);
static int start_genie(struct Room *room, int fd, int status);
static int end_genie(struct Room *room, int fd, int status);

static int dock_undock_subway(struct Room *room, int fd, int status);
static int start_subway(struct Room *room, int fd, int status);
static int end_subway(struct Room *room, int fd, int status);

static int respawn_boss(struct Room *room, int fd, int status);

static void map_kill_monster(struct Map *map, uint32_t oid);
static bool map_calculate_drop_position(struct Map *map, struct Point *p);
static void map_destroy_reactor(struct Map *map, uint32_t oid);
static int map_drop_batch_from_map_object(struct Map *map, struct MapPlayer *player, struct MapObject *object, size_t count, struct Drop *drops);
static bool do_client_auto_pickup(struct Map *map, struct Client *client, struct Drop *drop);

static void on_next_drop(struct Room *room, struct TimerHandle *handle);
static void on_exclusive_drop_time_expired(struct Room *room, struct TimerHandle *handle);
static void on_drop_time_expired(struct Room *room, struct TimerHandle *handle);
static void on_respawn(struct Room *room, struct TimerHandle *handle);
static void on_respawn_reactor(struct Room *room, struct TimerHandle *handle);

struct Map *map_create(struct ChannelServer *server, struct Room *room, struct ScriptManager *reactor_manager)
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
            struct MapObject *object = object_list_allocate(&map->objectList);
            object->type = MAP_OBJECT_NPC;
            object->index = map->npcCount;
            map->npcs[map->npcCount].oid = object->oid;
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

    map->monsters = malloc(map->spawnerCount * sizeof(struct MapMonster));
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

    map->occupiedSeats = calloc(wz_get_map_seat_count(room_get_id(room)), sizeof(bool));
    if (map->occupiedSeats == NULL) {
        free(map->monsters);
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
        struct MapObject *obj = object_list_allocate(&map->objectList);
        obj->type = MAP_OBJECT_MONSTER;
        obj->index = i;
        map->monsters[i].monster.oid = obj->oid;
        map->monsters[i].monster.id = map->spawners[i].id;
        map->monsters[i].monster.x = map->spawners[i].x;
        map->monsters[i].monster.y = map->spawners[i].y;
        map->monsters[i].monster.fh = map->spawners[i].fh;
        map->monsters[i].monster.hp = wz_get_monster_stats(map->spawners[i].id)->hp;
        map->monsters[i].spawnerIndex = i;
        map->monsters[i].controller = NULL;
    }
    map->monsterCapacity = map->spawnerCount;
    map->monsterCount = map->spawnerCount;

    for (size_t i = 0; i < map->reactorCount; i++) {
        struct MapObject *obj = object_list_allocate(&map->objectList);
        obj->type = MAP_OBJECT_REACTOR;
        obj->index = i;
        map->reactors[i].oid = obj->oid;
        map->reactors[i].id = reactors_info[i].id;
        map->reactors[i].x = reactors_info[i].pos.x;
        map->reactors[i].y = reactors_info[i].pos.y;
        map->reactors[i].state = 0;
        map->reactors[i].keepAlive = false;
    }

    map->boss.monster.oid = -1;

    uint32_t id = room_get_id(room);
    if (id == 101000300 || id == 200000111 ) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, dock_undock_boat);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 101000301 || id == 200000112) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, start_sailing);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id / 10 == 20009001 || id / 10 == 20009000) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, end_sailing);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 200000121 || id == 220000110) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, dock_undock_train);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_TRAIN), EVENT_TRAIN_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 200000122 || id == 220000111) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, start_train);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_TRAIN), EVENT_TRAIN_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 200090100 || id == 200090110) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, end_train);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_TRAIN), EVENT_TRAIN_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 200000151 || id == 260000100) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, dock_undock_genie);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_GENIE), EVENT_GENIE_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 200000152 || id == 260000110) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, start_genie);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_GENIE), EVENT_GENIE_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 200090400 || id == 200090410) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, end_genie);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_GENIE), EVENT_GENIE_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 103000100 || id == 600010001) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, dock_undock_subway);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_SUBWAY), EVENT_SUBWAY_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 600010004 || id == 600010002) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, start_subway);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_SUBWAY), EVENT_SUBWAY_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 600010005 || id == 600010003) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, end_subway);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_SUBWAY), EVENT_SUBWAY_PROPERTY_SAILING, on_boat_state_changed, map);
    } else if (id == 100040105 || id == 100040106 || id == 101030404 || id == 104000400 || id == 105090310 ||
            id == 107000300 || id == 110040000 || id == 200010300 || id == 220050000 || id == 220050100 ||
            id == 220050200 || id == 221040301 || id == 222010310 || id == 230020100 || id == 240040401 ||
            id == 250010304 || id == 250010504 || id == 251010102 || id == 260010201 || id == 261030000 ||
            id == 677000001 || id == 677000003 || id == 677000005 || id == 677000007 || id == 677000009 || id == 677000012) {
        map->fd = eventfd(0, 0);
        room_set_event(room, map->fd, POLLIN, respawn_boss);
        map->listener = event_add_listener(channel_server_get_event(server, EVENT_AREA_BOSS), EVENT_AREA_BOSS_PROPERTY_RESET, on_boat_state_changed, map);

        if (!event_area_boss_register(id)) {
            room_keep_alive(room);

            struct Point p;

            switch (id) {
            case 100040105:
                map->bossSpawner.id = 5220002;
                p.x = 456;
                p.y = 278;
            break;

            case 100040106:
                map->bossSpawner.id = 5220002;
                p.x = 474;
                p.y = 278;
            break;

            case 101030404:
                map->bossSpawner.id = 3220000;
                p.x = 800;
                p.y = 1280;
            break;

            case 104000400:
                map->bossSpawner.id = 2220000;
                p.x = 279;
                p.y = -496;
            break;

            case 105090310:
                // TODO: Pick from the 2 spawn points randomly
                map->bossSpawner.id = 8220008;
                p.x = -626;
                p.y = -604;
                //p.x = 735;
                //p.y = -600;
            break;

            case 107000300:
                map->bossSpawner.id = 6220000;
                p.x = 90;
                p.y = 119;
            break;

            case 110040000:
                map->bossSpawner.id = 5220001;
                p.x = -400;
                p.y = 140;
            break;

            case 200010300:
                map->bossSpawner.id = 8220000;
                p.x = 208;
                p.y = 83;
            break;

            case 220050000:
                map->bossSpawner.id = 5220003;
                p.x = -300;
                p.y = 1030;
            break;

            case 220050100:
                map->bossSpawner.id = 5220003;
                p.x = -385;
                p.y = 1030;
            break;

            case 220050200:
                map->bossSpawner.id = 5220003;
                p.x = 0;
                p.y = 1030;
            break;

            case 221040301:
                map->bossSpawner.id = 6220001;
                p.x = -4224;
                p.y = 776;
            break;

            case 222010310:
                map->bossSpawner.id = 7220001;
                p.x = -150;
                p.y = 33;
            break;

            case 230020100:
                map->bossSpawner.id = 4220001;
                p.x = -350;
                p.y = 520;
            break;

            case 240040401:
                map->bossSpawner.id = 8220003;
                p.x = 0;
                p.y = 1125;
            break;

            case 250010304:
                map->bossSpawner.id = 7220000;
                p.x = -450;
                p.y = 390;
            break;

            case 250010504:
                map->bossSpawner.id = 7220002;
                p.x = 150;
                p.y = 540;
            break;

            case 251010102:
                map->bossSpawner.id = 5220004;
                p.x = 560;
                p.y = 50;
            break;

            case 260010201:
                map->bossSpawner.id = 3220001;
                p.x = 645;
                p.y = 275;
            break;

            case 261030000:
                map->bossSpawner.id = 8220002;
                p.x = -450;
                p.y = 180;
            break;

            case 677000001:
                map->bossSpawner.id = 9400612;
                p.x = 461;
                p.y = 61;
            break;

            case 677000003:
                map->bossSpawner.id = 9400610;
                p.x = 467;
                p.y = 0;
            break;

            case 677000005:
                map->bossSpawner.id = 9400609;
                p.x = 201;
                p.y = 80;
            break;

            case 677000007:
                map->bossSpawner.id = 9400611;
                p.x = 171;
                p.y = 50;
            break;

            case 677000009:
                map->bossSpawner.id = 9400613;
                p.x = 251;
                p.y = -841;
            break;

            case 677000012:
                map->bossSpawner.id = 9400633;
                p.x = 842;
                p.y = 0;
            break;
            }

            map->bossSpawner.x = p.x;
            map->bossSpawner.y = p.y;
            map->bossSpawner.fh = foothold_tree_find_below(map->footholdTree, &p)->id;

            struct MapObject *obj = object_list_allocate(&map->objectList);
            obj->type = MAP_OBJECT_BOSS;
            map->boss.controller = NULL;
            map->boss.monster.oid = obj->oid;
            map->boss.monster.id = map->bossSpawner.id;
            map->boss.monster.x = map->bossSpawner.x;
            map->boss.monster.y = map->bossSpawner.y;
            map->boss.monster.fh = map->bossSpawner.fh;
            map->boss.monster.hp = wz_get_monster_stats(map->bossSpawner.id)->hp;
        }
    }

    map->server = server;

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
    uint32_t id = room_get_id(map->room);
    if (id == 101000300 || id == 101000301 || id / 10 == 20009001 || id == 200000111 || id == 200000112 || id / 10 == 20009000) {
        event_remove_listener(channel_server_get_event(map->server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING, map->listener);
        room_close_event(map->room);
        close(map->fd);
    } else if (id == 200000121 || id == 220000110 || id == 200000122 || id == 220000111 || id == 200090100 || id == 200090110) {
        event_remove_listener(channel_server_get_event(map->server, EVENT_TRAIN), EVENT_TRAIN_PROPERTY_SAILING, map->listener);
        room_close_event(map->room);
        close(map->fd);
    } else if (id == 200000151 || id == 260000100 || id == 200000152 || id == 260000110 || id == 200090400 || id == 200090410) {
        event_remove_listener(channel_server_get_event(map->server, EVENT_GENIE), EVENT_GENIE_PROPERTY_SAILING, map->listener);
        room_close_event(map->room);
        close(map->fd);
    } else if (id == 103000100 || id == 600010001 || id == 600010004 || id == 600010002 || id == 600010005 || id == 600010003) {
        event_remove_listener(channel_server_get_event(map->server, EVENT_SUBWAY), EVENT_SUBWAY_PROPERTY_SAILING, map->listener);
        room_close_event(map->room);
        close(map->fd);
    } else if (id == 100040105 || id == 100040106 || id == 101030404 || id == 104000400 || id == 105090310 ||
            id == 107000300 || id == 110040000 || id == 200010300 || id == 220050000 || id == 220050100 ||
            id == 220050200 || id == 221040301 || id == 222010310 || id == 230020100 || id == 240040401 ||
            id == 250010304 || id == 250010504 || id == 251010102 || id == 260010201 || id == 261030000 ||
            id == 677000001 || id == 677000003 || id == 677000005 || id == 677000007 || id == 677000009 || id == 677000012) {
        event_remove_listener(channel_server_get_event(map->server, EVENT_AREA_BOSS), EVENT_AREA_BOSS_PROPERTY_RESET, map->listener);
        room_close_event(map->room);
        close(map->fd);
    }

    free(map->occupiedSeats);
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

    if (room_get_id(map->room) == 101000301 || room_get_id(map->room) == 200000112) {
        if (event_get_property(channel_server_get_event(map->server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING) == 2) {
            client_warp(client, room_get_id(map->room) == 101000301 ? 200090010 : 200090000, 0);
            return -2;
        }
    } else if (room_get_id(map->room) / 10 == 20009001 || room_get_id(map->room) / 10 == 20009000) {
        if (event_get_property(channel_server_get_event(map->server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING) != 2) {
            client_warp(client, room_get_id(map->room) / 10 == 20009001 ? 200000100 : 101000300, 0);
            return -2;
        }
    }

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

    client_announce_self_to_map(client);

    for (size_t i = 0; i < map->playerCount; i++)
        client_announce_add_player(client, client_get_character(map->players[i].client));

    for (size_t i = 0; i < map->npcCount; i++)
        client_announce_add_npc(client, &map->npcs[i]);

    if (map->heap.count == 0)
        map->respawnHandle = room_add_timer(map->room, 10 * 1000, on_respawn, NULL, false);

    // TODO: Maybe allocate it to `map->monsterCapacity` size
    player->player->monsters = malloc(map->spawnerCount * sizeof(struct MapMonster *));
    if (player->player->monsters == NULL) {
        if (map->heap.count == 0)
            room_stop_timer(map->respawnHandle);
        return -1;
    }

    player->player->monsterCount = 0;
    if (map->heap.count == 0) {
        for (size_t i = 0; i < map->monsterCount; i++) {
            if (map->monsters[i].monster.hp > 0) {
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
        const struct MapMonster *monster = &map->monsters[i];
        client_announce_monster(client, &monster->monster);
    }

    for (size_t i = 0; i < player->player->monsterCount; i++) {
        const struct Monster *monster = &player->player->monsters[i]->monster;
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
            client_announce_spawn_drop(client, 0, 0, map->dropBatches[i].exclusive ? 1 : 2, false, drop);
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
            // Dropping batches can only be exclusive as they must come from a monster or a reactor
            client_announce_spawn_drop(client, 0, 0, 1, false, drop);
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

    if (map->boss.monster.oid != -1) {
        client_announce_monster(client, &map->boss.monster);
        if (map->boss.controller == NULL) {
            map->boss.controller = player->player;
            const struct Monster *monster = &map->boss.monster;
            uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
            spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
            session_write(session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
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
                struct Monster *monster = &next->controller->monsters[i]->monster;
                uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
                spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
                session_write(client_get_session(next->controller->client), SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }

            if (map->boss.monster.oid != -1 && map->boss.controller == player) {
                map->boss.controller = next->controller;
                struct Monster *monster = &map->boss.monster;
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

            // Since this is the last player, they must have the control over the boss
            map->boss.controller = NULL;

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
    if (object == NULL || (object->type != MAP_OBJECT_MONSTER && object->type != MAP_OBJECT_BOSS))
        return false;

    struct Monster *monster = object->type == MAP_OBJECT_MONSTER ? &map->monsters[object->index].monster : &map->boss.monster;
    if (monster->id != id)
        return false;

    return monster->hp > 0;
}

uint32_t map_damage_monster_by(struct Map *map, struct MapPlayer *player, uint32_t char_id, uint32_t oid, size_t hit_count, int32_t *damage)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || (object->type != MAP_OBJECT_MONSTER && object->type != MAP_OBJECT_BOSS))
        return -1;

    struct MapMonster *monster = object->type == MAP_OBJECT_MONSTER ? &map->monsters[object->index] : &map->boss;

    if (monster->monster.hp > 0) {
        if (monster->controller != player) {
            // Switch the control of the monster
            struct MapPlayer *old = monster->controller;
            if (object->type == MAP_OBJECT_MONSTER) {
                old->monsterCount--;
                old->monsters[monster->indexInController] = old->monsters[old->monsterCount];
                old->monsters[monster->indexInController]->indexInController = monster->indexInController;
                monster->controller = player;
                monster->indexInController = player->monsterCount;
                player->monsters[player->monsterCount] = monster;
                player->monsterCount++;
            } else {
                map->boss.controller = player;
            }

            {
                uint8_t packet[REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH];
                remove_monster_controller_packet(oid, packet);
                session_write(client_get_session(old->client), REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }

            {
                uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
                spawn_monster_controller_packet(oid, false, monster->monster.id, monster->monster.x, monster->monster.y, monster->monster.fh, false, packet);
                session_write(client_get_session(player->client), SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }
        }

        for (size_t i = 0; i < hit_count && monster->monster.hp > 0; i++)
            monster->monster.hp -= damage[i];

        if (monster->monster.hp < 0)
            monster->monster.hp = 0;

        {
            uint8_t packet[MONSTER_HP_PACKET_LENGTH];
            monster_hp_packet(monster->monster.oid, monster->monster.hp * 100 / wz_get_monster_stats(monster->monster.id)->hp, packet);
            session_write(client_get_session(player->client), MONSTER_HP_PACKET_LENGTH, packet);
        }

        if (monster->monster.hp == 0) {
            const struct MonsterDropInfo *info = drop_info_find(monster->monster.id);

            // Remove the controller from the monster
            // TODO: Why is there a NULL check here?
            if (monster->controller != NULL) {
                if (object->type == MAP_OBJECT_MONSTER) {
                    monster->controller->monsters[monster->indexInController] = monster->controller->monsters[monster->controller->monsterCount - 1];
                    monster->controller->monsters[monster->indexInController]->indexInController = monster->indexInController;
                    monster->controller->monsterCount--;
                    monster->controller = NULL;
                } else {
                    monster->controller = NULL;
                }
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
                                drops[drop_count].item.id = 0;
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
                            drops[drop_count].item.id = 0;
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
                    map_kill_monster(map, monster->monster.oid);
            } else {
                map_kill_monster(map, monster->monster.oid);
            }

            return monster->monster.id;
        }
    }

    return -1;
}

uint32_t *map_kill_all_by(struct Map *map, struct MapPlayer *player, size_t *count)
{
    uint32_t *ids = malloc(map->monsterCount * sizeof(uint32_t));
    if (ids == NULL)
        return NULL;

    *count = 0;

    for (size_t i = 0; i < map->monsterCount; i++) {
        if (map->monsters[i].monster.hp > 0) {
            uint32_t oid = map->monsters[i].monster.oid;
            uint32_t id = map_damage_monster_by(map, player, client_get_character(player->client)->id, oid, 1, &map->monsters[i].monster.hp);
            ids[*count] = id;
            (*count)++;

            // If the OID of the monster doesn't exist anymore then map_kill_monster was called
            // And so the current monster index was replaced with the last monster
            if (object_list_get(&map->objectList, oid) == NULL)
                i--;
        }
    }

    return ids;
}

struct ClientResult map_hit_reactor(struct Map *map, struct MapPlayer *player, uint32_t oid, uint8_t stance)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_REACTOR)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    struct Reactor *reactor = &map->reactors[object->index];
    const struct ReactorInfo *info = wz_get_reactor_info(reactor->id);
    if (info->states[reactor->state].eventCount == 0)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    bool found = false;
    for (size_t i = 0; i < info->states[reactor->state].eventCount; i++) {
        if (info->states[reactor->state].events[i].type == REACTOR_EVENT_TYPE_HIT) {
            reactor->state = info->states[reactor->state].events[i].next;
            found = true;
            break;
        }
    }

    if (!found)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    if (info->states[reactor->state].eventCount == 0) {
        // Reactor broken
        char script_name[37];
        strcpy(script_name, info->action);
        strcat(script_name, ".lua");
        player->script = script_manager_alloc(map->reactorManager, script_name, 0);
        if (player->script == NULL) {
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
        }

        player->rm = reactor_manager_create(map, player->client, object->index, oid);

        enum ScriptResult res = script_manager_run(player->script, SCRIPT_REACTOR_MANAGER_TYPE, player->rm);

        switch (res) {
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
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        case SCRIPT_RESULT_VALUE_NEXT:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
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

uint32_t map_get_npc(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_NPC)
        return -1;
    return map->npcs[object->index].id;
}

bool map_move_monster(struct Map *map, struct MapPlayer *controller, uint8_t activity, uint32_t oid, int16_t x, int16_t y, uint16_t fh, uint8_t stance, size_t len, uint8_t *raw_data)
{
    struct MapObject *obj = object_list_get(&map->objectList, oid);
    if (obj == NULL || (obj->type != MAP_OBJECT_MONSTER && obj->type != MAP_OBJECT_BOSS))
        return false;

    struct MapMonster *monster = obj->type == MAP_OBJECT_MONSTER ? &map->monsters[obj->index] : &map->boss;
    if (monster->monster.hp <= 0 || monster->controller != controller)
        return false;

    monster->monster.x = x;
    monster->monster.y = y;
    monster->monster.fh = fh;
    monster->monster.stance = stance;

    return true;
}

int map_drop_batch_from_reactor(struct Map *map, struct MapPlayer *player, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object == NULL || object->type != MAP_OBJECT_REACTOR)
        return -1;

    const struct MapReactorInfo *reactor = &wz_get_reactors_for_map(map_get_id(map), NULL)[object->index];
    const struct MonsterDropInfo *info = reactor_drop_info_find(reactor->id);

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
            enum DropType type = drops_copy[i].itemId == 0 ? DROP_TYPE_MESO :
                (drops_copy[i].itemId / 1000000 == 1 ? DROP_TYPE_EQUIP : DROP_TYPE_ITEM);
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
                    drops[drop_count].item.id = 0;
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
                drops[drop_count].item.id = 0;
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

    return map_drop_batch_from_map_object(map, player, object, drop_count, drops);
}

static int map_drop_batch_from_map_object(struct Map *map, struct MapPlayer *player, struct MapObject *object, size_t count, struct Drop *drops)
{
    struct Point pos;
    if (object->type == MAP_OBJECT_MONSTER) {
        pos.x = map->monsters[object->index].monster.x;
        pos.y = map->monsters[object->index].monster.y;
    } else if (object->type == MAP_OBJECT_BOSS) {
        pos.x = map->boss.monster.x;
        pos.y = map->boss.monster.y;
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

        batch->timer = room_add_timer(map->room, 200, on_next_drop, batch, true);

        // Drop the first one immediatly
        // Also can't be player drop as they come in 1's
        struct Drop *drop = &drops[0];
        struct MapObject *drop_object = object_list_allocate(&map->objectList);
        drop->oid = drop_object->oid;
        drop_object->type = MAP_OBJECT_DROPPING;
        drop_object->index = map->droppingBatchCount;
        drop_object->index2 = 0;

        for (size_t i = 0; i < map->playerCount; i++)
            client_announce_drop(map->players[i].client, client_get_character(player->client)->id, object_copy.oid, 1, false, drop);

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
        batch->timer = room_add_timer(map->room, 15 * 1000, on_exclusive_drop_time_expired, NULL, true);

        batch->drops = malloc(sizeof(struct Drop));
        *batch->drops = *drops;
        struct Drop *drop = &batch->drops[0];
        struct MapObject *object = object_list_allocate(&map->objectList);
        drop->oid = object->oid;
        object->type = MAP_OBJECT_DROP;
        object->index = map->dropBatchEnd;
        object->index2 = 0;
        batch->count = 1;
        batch->owner = player;
        batch->indexInPlayer = player->dropCount;
        batch->ownerId = client_get_character(player->client)->id;
        batch->exclusive = true;
        player->drops[player->dropCount] = batch;
        player->dropCount++;
        map->dropBatchEnd++;

        switch (drop->type) {
        case DROP_TYPE_MESO: {
            uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
            drop_meso_from_object_packet(drop->oid, drop->meso, batch->ownerId, 1, drop->x, drop->y, drop->x, drop->y, object_copy.oid, false, packet);
            room_broadcast(map->room, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
        }
            break;

        case DROP_TYPE_ITEM: {
            for (size_t i = 0; i < map->playerCount; i++)
                client_announce_drop(map->players[i].client, batch->ownerId, object_copy.oid, 1, false, drop);
        }
        break;

        case DROP_TYPE_EQUIP: {
            // As far as I know, no equip item is quest-exclusive
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, batch->ownerId, 1, drop->x, drop->y, drop->x, drop->y, object_copy.oid, false, packet);
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
        // We need to cache this `count`
        // as `batch` can be invalidated during a call to map_remove_drop() in do_client_auto_pickup()
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
    batch->timer = room_add_timer(map->room, 300 * 1000, on_drop_time_expired, NULL, true);

    batch->drops = malloc(sizeof(struct Drop));
    *batch->drops = *drop;
    drop = &batch->drops[0];
    struct MapObject *object = object_list_allocate(&map->objectList);
    drop->oid = object->oid;
    object->type = MAP_OBJECT_DROP;
    object->index = map->dropBatchEnd;
    object->index2 = 0;
    batch->count = 1;
    batch->owner = player;
    batch->ownerId = client_get_character(player->client)->id;
    batch->indexInPlayer = player->dropCount;
    batch->exclusive = false;
    player->drops[player->dropCount] = batch;
    player->dropCount++;
    map->dropBatchEnd++;

    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
        drop_meso_from_object_packet(drop->oid, drop->meso, batch->ownerId, 2, drop->x, drop->y, drop->x, drop->y, batch->ownerId, true, packet);
        room_broadcast(map->room, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_ITEM: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->item.item.itemId, batch->ownerId, 2, drop->x, drop->y, drop->x, drop->y, batch->ownerId, true, packet);
        room_broadcast(map->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_EQUIP: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, batch->ownerId, 2, drop->x, drop->y, drop->x, drop->y, batch->ownerId, true, packet);
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

bool map_player_can_pick_up_drop(struct Map *map, struct MapPlayer *player, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);

    if (object->type == MAP_OBJECT_DROPPING)
        return map->droppingBatches[object->index]->ownerId == client_get_character(player->client)->id;

    struct DropBatch *batch = &map->dropBatches[object->index];
    return batch->ownerId == client_get_character(player->client)->id || !batch->exclusive;
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

bool map_try_occupy_seat(struct Map *map, uint16_t id)
{
    if (!map->occupiedSeats[id]) {
        map->occupiedSeats[id] = true;
        return true;
    }

    return false;
}

void map_tire_seat(struct Map *map, uint16_t id)
{
    map->occupiedSeats[id] = false;
}

static void on_boat_state_changed(void *ctx)
{
    struct Map *map = ctx;
    uint64_t one = 1;
    write(map->fd, &one, sizeof(uint64_t));
}

static int dock_undock_boat(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING);
    if (state != 1) {
        uint8_t packet[BOAT_PACKET_LENGTH];
        if (state == 2) {
            boat_packet(false, packet);
        } else if (state == 0) {
            boat_packet(true, packet);
        }
        room_broadcast(room, BOAT_PACKET_LENGTH, packet);
    }

    return 0;
}

static int start_sailing(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING);
    if (state == 2) {
        // client_warp() calls map_leave() which in turn swaps the last player in the array
        // with the current one so we need to decrease i to check the same index again
        for (size_t i = 0; i < map->playerCount; i++) {
            client_close_script(map->players[i].client);
            client_warp(map->players[i].client, room_get_id(room) == 101000301 ? 200090010 : 200090000, 0);
            i--;
        }
    }

    return 0;
}

static int end_sailing(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_BOAT), EVENT_BOAT_PROPERTY_SAILING);
    if (state == 0) {
        // There is no suspending script when on the sail map so client_close_script() is unnecessary
        for (size_t i = 0; i < map->playerCount; i++) {
            client_warp(map->players[i].client, room_get_id(room) / 10 == 20009001 ? 200000100 : 101000300, 0);
            i--;
        }
    }

    return 0;

}

static int dock_undock_train(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_TRAIN), EVENT_TRAIN_PROPERTY_SAILING);
    if (state != 1) {
        uint8_t packet[BOAT_PACKET_LENGTH];
        if (state == 2) {
            boat_packet(false, packet);
        } else if (state == 0) {
            boat_packet(true, packet);
        }
        room_broadcast(room, BOAT_PACKET_LENGTH, packet);
    }

    return 0;
}

static int start_train(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_TRAIN), EVENT_TRAIN_PROPERTY_SAILING);
    if (state == 2) {
        for (size_t i = 0; i < map->playerCount; i++) {
            client_close_script(map->players[i].client);
            client_warp(map->players[i].client, room_get_id(room) == 200000122 ? 200090100 : 200090110, 0);
            i--;
        }
    }

    return 0;
}

static int end_train(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_TRAIN), EVENT_TRAIN_PROPERTY_SAILING);
    if (state == 0) {
        for (size_t i = 0; i < map->playerCount; i++) {
            client_warp(map->players[i].client, room_get_id(room) == 200090100 ? 220000110 : 200000100, 0);
            i--;
        }
    }

    return 0;
}

static int dock_undock_genie(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_GENIE), EVENT_GENIE_PROPERTY_SAILING);
    if (state != 1) {
        uint8_t packet[BOAT_PACKET_LENGTH];
        if (state == 2) {
            boat_packet(false, packet);
        } else if (state == 0) {
            boat_packet(true, packet);
        }
        room_broadcast(room, BOAT_PACKET_LENGTH, packet);
    }

    return 0;
}

static int start_genie(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_GENIE), EVENT_GENIE_PROPERTY_SAILING);
    if (state == 2) {
        for (size_t i = 0; i < map->playerCount; i++) {
            client_close_script(map->players[i].client);
            client_warp(map->players[i].client, room_get_id(room) == 200000152 ? 200090400 : 200090410, 0);
            i--;
        }
    }

    return 0;
}

static int end_genie(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_GENIE), EVENT_GENIE_PROPERTY_SAILING);
    if (state == 0) {
        for (size_t i = 0; i < map->playerCount; i++) {
            client_warp(map->players[i].client, room_get_id(room) == 200090400 ? 260000100 : 200000100, 0);
            i--;
        }
    }

    return 0;
}

static int dock_undock_subway(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_SUBWAY), EVENT_SUBWAY_PROPERTY_SAILING);
    if (state != 1) {
        uint8_t packet[PLAY_SOUND_PACKET_MAX_LENGTH];
        const char *sound = "subway/whistle";
        size_t len = play_sound_packet(strlen(sound), sound, packet);
        room_broadcast(room, len, packet);
    }

    return 0;
}

static int start_subway(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_SUBWAY), EVENT_SUBWAY_PROPERTY_SAILING);
    if (state == 2) {
        for (size_t i = 0; i < map->playerCount; i++) {
            client_close_script(map->players[i].client);
            client_warp(map->players[i].client, room_get_id(room) == 600010004 ? 600010005 : 600010003, 0);
            i--;
        }
    }

    return 0;
}

static int end_subway(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    int32_t state = event_get_property(channel_server_get_event(map->server, EVENT_SUBWAY), EVENT_SUBWAY_PROPERTY_SAILING);
    if (state == 0) {
        for (size_t i = 0; i < map->playerCount; i++) {
            client_close_script(map->players[i].client);
            client_warp(map->players[i].client, room_get_id(room) == 600010005 ? 600010001 : 103000100, 0);
            i--;
        }
    }

    return 0;
}

static int respawn_boss(struct Room *room, int fd, int status)
{
    struct Map *map = room_get_context(room);
    uint64_t one;
    read(fd, &one, sizeof(uint64_t));
    event_area_boss_register(room_get_id(room));
    if (map->boss.monster.oid == -1) {
        room_keep_alive(room);

        struct MapObject *obj = object_list_allocate(&map->objectList);
        obj->type = MAP_OBJECT_BOSS;

        struct ControllerHeapNode *next = heap_top(&map->heap);

        map->boss.controller = next != NULL ? next->controller : NULL;
        map->boss.monster.oid = obj->oid;
        map->boss.monster.x = map->bossSpawner.x;
        map->boss.monster.y = map->bossSpawner.y;
        map->boss.monster.fh = map->bossSpawner.fh;
        map->boss.monster.hp = wz_get_monster_stats(map->boss.monster.id)->hp;

        struct Monster *monster = &map->boss.monster;
        for (size_t i = 0; i < map->playerCount; i++) {
            uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
            spawn_monster_packet(monster->oid, monster->id, monster->x, monster->y, monster->fh, true, packet);
            room_broadcast(map->room, SPAWN_MONSTER_PACKET_LENGTH, packet);
        }

        if (next != NULL) {
            uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
            spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, true, packet);
            session_write(client_get_session(next->controller->client), SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
        }

        const char *msg;
        switch (map->boss.monster.id) {
        case 2220000:
            msg = "A cool breeze was felt when Mano appeared.";
        break;

        case 3220000:
            msg = "Stumpy has appeared with a stumping sound that rings the Stone Mountain.";
        break;

        case 3220001:
            msg = "Deo slowly appeared out of the sand dust.";
        break;

        case 4220001:
            msg = "A strange shell has appeared from a grove of seaweed";
        break;

        case 5220001:
            msg = "A strange turban shell has appeared on the beach.";
        break;

        case 5220002:
            msg = "Faust appeared amidst the blue fog.";
        break;

        case 5220003:
            msg = "Tick-Tock Tick-Tock! Timer makes it's presence known.";
        break;

        case 5220004:
            msg = "From the mists surrounding the herb garden, the gargantuous Giant Centipede appears.";
        break;

        case 6220000:
            msg = "The huge crocodile Dyle has come out from the swamp.";
        break;

        case 6220001:
            msg = "Zeno has appeared with a heavy sound of machinery.";
        break;

        case 7220000:
            msg = "Tae Roon has appeared with a soft whistling sound.";
        break;

        case 7220001:
            msg = "As the moon light dims, a long fox cry can be heard and the presence of the old fox can be felt";
        break;

        case 7220002:
            msg = "The ghostly air around here has become stronger. The unpleasant sound of a cat crying can be heard.";
        break;

        case 8220000:
            msg = "Eliza has appeared with a black whirlwind.";
        break;

        case 8220002:
            msg = "Kimera has appeared out of the darkness of the underground with a glitter in her eyes.";
        break;

        case 8220003:
            msg = "Leviathan emerges from the canyon and the cold icy wind blows.";
        break;

        case 8220008:
            msg = "Slowly, a suspicious food stand opens up on a strangely remote place.";
        break;

        case 9400609:
            msg = "Andras has appeared";
        break;

        case 9400610:
            msg = "Amdusias has appeared";
        break;

        case 9400611:
            msg = "Crocell has appeared";
        break;

        case 9400612:
            msg = "Marbas has appeared";
        break;

        case 9400613:
            msg = "Valefor has appeared";
        break;

        case 9400633:
            msg = "Astaroth has appeared";
        break;
        }
        uint8_t packet[SERVER_NOTICE_PACKET_MAX_LENGTH];
        size_t len = server_notice_packet(strlen(msg), msg, packet);
        room_broadcast(room, len, packet);
    }

    return 0;
}

static void map_kill_monster(struct Map *map, uint32_t oid)
{
    struct MapObject *object = object_list_get(&map->objectList, oid);
    if (object->type == MAP_OBJECT_MONSTER) {
        struct MapMonster *monster = &map->monsters[object->index];
        if (monster->spawnerIndex != -1) {
            map->dead[map->deadCount] = monster->spawnerIndex;
            map->deadCount++;
        }
        map->monsters[object->index] = map->monsters[map->monsterCount - 1];
        if (map->monsters[object->index].controller != NULL)
            map->monsters[object->index].controller->monsters[map->monsters[object->index].indexInController] = &map->monsters[object->index];
        object_list_get(&map->objectList, map->monsters[object->index].monster.oid)->index = object->index;
        map->monsterCount--;
        object_list_free(&map->objectList, oid);
        uint8_t packet[KILL_MONSTER_PACKET_LENGTH];
        kill_monster_packet(oid, true, packet);
        room_broadcast(map->room, KILL_MONSTER_PACKET_LENGTH, packet);
    } else { // MAP_OBJECT_BOSS
        object_list_free(&map->objectList, map->boss.monster.oid);
        map->boss.monster.oid = -1;
        uint8_t packet[KILL_MONSTER_PACKET_LENGTH];
        kill_monster_packet(oid, true, packet);
        room_broadcast(map->room, KILL_MONSTER_PACKET_LENGTH, packet);
    }
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

    struct MapObject *object = object_list_allocate(&map->objectList);
    drop->oid = object->oid;
    object->type = MAP_OBJECT_DROPPING;
    object->index = batch->index;
    object->index2 = batch->current;

    for (size_t i = 0; i < map->playerCount; i++)
        client_announce_drop(map->players[i].client, batch->ownerId, batch->dropperOid, 1, false, drop);

    batch->current++;
    if (batch->current < batch->count) {
        if (batch->owner != NULL && client_is_auto_pickup_enabled(batch->owner->client))
            do_client_auto_pickup(map, batch->owner->client, drop);
        batch->timer = room_add_timer(map->room, 200, on_next_drop, batch, true);
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

        if (batch->owner != NULL && batch->owner->dropCount == batch->owner->dropCapacity) {
            void *temp = realloc(batch->owner->drops, (batch->owner->dropCapacity * 2) * sizeof(struct DropBatch *));
            if (temp == NULL)
                return;

            batch->owner->drops = temp;
            batch->owner->dropCapacity *= 2;
        }

        struct DropBatch *new = &map->dropBatches[map->dropBatchEnd];
        new->timer = room_add_timer(map->room, 15 * 1000, on_exclusive_drop_time_expired, NULL, true);
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
        new->exclusive = true;
        map->dropBatchEnd++;

        object = object_list_get(&map->objectList, batch->dropperOid);
        if (object->type == MAP_OBJECT_REACTOR)
            map_destroy_reactor(map, batch->dropperOid);
        else // monster or boss
            map_kill_monster(map, batch->dropperOid);

        if (batch->owner != NULL && client_is_auto_pickup_enabled(batch->owner->client))
            do_client_auto_pickup(map, batch->owner->client, drop);

        free(batch);
    }
}

static void on_exclusive_drop_time_expired(struct Room *room, struct TimerHandle *handle)
{
    struct Map *map = room_get_context(room);
    struct DropBatch *batch = &map->dropBatches[map->dropBatchStart];
    while (!batch->exclusive)
        batch++;

    batch->exclusive = false;

    batch->timer = room_add_timer(room, 285 * 1000, on_drop_time_expired, NULL, true);
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

#define MIN(x, y) ((x) < (y) ? (x) : (y))

static void on_respawn(struct Room *room, struct TimerHandle *handle)
{
    struct Map *map = room_get_context(room);

    size_t maxSpawnCount = ceil((0.7 + (0.05 * MIN(6, map->playerCount))) * map->spawnerCount);

    struct ControllerHeapNode *next = heap_top(&map->heap);
    size_t count = 0;
    while (map->monsterCount < maxSpawnCount) {

        if (map->monsterCount == map->monsterCapacity) {
            struct MapMonster *temp = realloc(map->monsters, (map->monsterCapacity * 2) * sizeof(struct MapMonster));
            if (temp == NULL)
                return;

            map->monsters = temp;

            for (size_t i = 0; i < map->monsterCount; i++)
                map->monsters[i].controller->monsters[map->monsters[i].indexInController] = &map->monsters[map->monsterCount];

            map->monsterCapacity *= 2;
        }

        size_t i = rand() % map->deadCount;
        size_t temp = map->dead[i];
        map->dead[i] = map->dead[map->deadCount - 1];
        i = temp;

        struct MapObject *obj = object_list_allocate(&map->objectList);
        obj->type = MAP_OBJECT_MONSTER;
        obj->index = map->monsterCount;
        map->monsters[map->monsterCount].monster.oid = obj->oid;
        map->monsters[map->monsterCount].monster.id = map->spawners[i].id;
        map->monsters[map->monsterCount].monster.x = map->spawners[i].x;
        map->monsters[map->monsterCount].monster.y = map->spawners[i].y;
        map->monsters[map->monsterCount].monster.fh = map->spawners[i].fh;
        map->monsters[map->monsterCount].monster.hp = wz_get_monster_stats(map->spawners[i].id)->hp;
        map->monsters[map->monsterCount].spawnerIndex = i;
        map->monsters[map->monsterCount].controller = next->controller;
        map->monsters[map->monsterCount].indexInController = next->controller->monsterCount;
        next->controller->monsters[next->controller->monsterCount] = &map->monsters[map->monsterCount];
        next->controller->monsterCount++;
        map->monsterCount++;
        count++;
        map->deadCount--;
    }

    heap_inc(&map->heap, count);

    for (size_t i = map->monsterCount - count; i < map->monsterCount; i++) {
        const struct Monster *monster = &map->monsters[i].monster;
        uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
        spawn_monster_packet(monster->oid, monster->id, monster->x, monster->y, monster->fh, true, packet);
        room_broadcast(map->room, SPAWN_MONSTER_PACKET_LENGTH, packet);
    }

    for (size_t i = map->monsterCount - count; i < map->monsterCount; i++) {
        const struct Monster *monster = &map->monsters[i].monster;
        uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
        spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, true, packet);
        session_write(client_get_session(next->controller->client), SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
    }

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
    list->freeStackCapacity = 1;
    list->freeStackTop = 0;
    list->objects[0].type = MAP_OBJECT_NONE;

    return 0;
}

static void object_list_destroy(struct ObjectList *list)
{
    free(list->objects);
    free(list->freeStack);
}

static struct MapObject *object_list_allocate(struct ObjectList *list)
{
    // Highly unlikely, but just to make sure that we don't overflow oid
    if (list->objectCount == UINT16_MAX)
        return NULL;

    if (list->objectCount == list->objectCapacity) {
        struct MapObject *new = malloc((list->objectCapacity * 2) * sizeof(struct MapObject));
        if (new == NULL)
            return NULL;

        if (list->freeStackCapacity < list->objectCapacity * 2) {
            void *temp = realloc(list->freeStack, (list->objectCapacity * 2) * sizeof(uint32_t));
            if (temp == NULL) {
                free(new);
                return NULL;
            }

            list->freeStack = temp;
            list->freeStackCapacity *= 2;
        }

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

    oid |= 0xFFFF0000;

    size_t index = XXH3_64bits(&oid, sizeof(uint32_t)) % list->objectCapacity;
    while (list->objects[index].type != MAP_OBJECT_NONE && list->objects[index].type != MAP_OBJECT_DELETED) {
        index++;
        index %= list->objectCapacity;
    }

    list->objects[index].type = MAP_OBJECT_NONE;
    list->objects[index].oid = oid;

    list->objectCount++;
    return &list->objects[index];
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
    list->freeStack[list->freeStackTop] = oid & 0xFFFF;
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
    while (list->objects[index].type == MAP_OBJECT_DELETED ||
            (list->objects[index].type != MAP_OBJECT_NONE && list->objects[index].oid != oid)) {
        index++;
        index %= list->objectCapacity;
        if (index == start)
            return NULL;
    }

    if (list->objects[index].type == MAP_OBJECT_NONE)
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
    if (node->index == 0 ||
            heap->controllers[(node->index-1) / 2]->controlleeCount < heap->controllers[node->index]->controlleeCount)
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

        if (heap->controllers[i]->controlleeCount <= heap->controllers[i*2 + 1]->controlleeCount &&
                heap->controllers[i]->controlleeCount <= heap->controllers[i*2 + 2]->controlleeCount) {
            break;
        }

        if (heap->controllers[i]->controlleeCount > heap->controllers[i*2 + 1]->controlleeCount &&
                heap->controllers[i]->controlleeCount > heap->controllers[i*2 + 2]->controlleeCount) {
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


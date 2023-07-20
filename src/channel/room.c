#include "room.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h> // iovec

#include "../hash-map.h"
#include "../packet.h"
#include "drops.h"
#include "events.h"
#include "map.h"
#include "scripting/reactor-manager.h"

struct Room {
    uint32_t id;
    //struct ChannelServer *server;
    struct Worker *worker;
    struct Listener *respawnListener;
    struct HashSetU32 *members;
    size_t timerCapacity;
    size_t timerCount;
    struct IoEvent **timers;
    bool keepAlive;
    struct Map *map;
};

static void room_broadcast(struct Room *room, size_t len, uint8_t *packet);

struct RoomMember {
    struct Room *room;
    struct Session *session;
    struct MapPlayer *player;
    struct HashSetU32 *questItems;
    struct HashSetU32 *visibleMapObjects;
    struct ScriptManager *reactorManager;
    struct ScriptInstance *script;
    struct ReactorManager *rm;
};

static size_t fixup_monster_oids(struct Room *room, size_t monster_count, size_t hit_count, uint32_t *oids, int32_t *damage);
static bool damage_monsters(struct Room *room, struct RoomMember *member, uint32_t *oids, uint8_t monster_count, uint8_t hit_count, int32_t *damage, size_t *count, uint32_t *ids);

struct IdMember {
    uint32_t id;
    struct RoomMember *member;
};

static void respawn(void *);

struct Room *room_create(struct Worker *worker, struct EventManager *mgr, uint32_t id)
{
    struct Room *room = malloc(sizeof(struct Room));
    if (room == NULL)
        return NULL;

    room->members = hash_set_u32_create(sizeof(struct IdMember), offsetof(struct IdMember, id));
    if (room->members == NULL) {
        free(room);
        return NULL;
    }

    room->map = map_create(id);
    if (room->map == NULL) {
        hash_set_u32_destroy(room->members);
        free(room);
        return NULL;
    }

    room->respawnListener = event_listen(event_manager_get_event(mgr, EVENT_GLOBAL_RESPAWN), 0, respawn, room);
    if (room->respawnListener == NULL) {
        map_destroy(room->map);
        hash_set_u32_destroy(room->members);
        free(room);
        return NULL;
    }

    room->id = id;
    room->worker = worker;
    room->timerCount = 0;

    return room;
}

static void do_respawn(struct Worker *worker, void *ctx);

static void respawn(void *ctx)
{
    struct Room *room = ctx;
    // Nothing that can be done if this fails
    worker_command(room->worker, do_respawn, room);
}

static void do_respawn(struct Worker *worker, void *ctx)
{
    struct Room *room = ctx;
    if (room->map == NULL)
        return;

    struct MapPlayer *controller;
    size_t count;
    struct MapMonster **monsters = NULL;
    if (!map_respawn(room->map, &controller, &count, &monsters))
        return; // TODO

    for (size_t i = 0; i < count; i++) {
        const struct Monster *monster = map_monster_get_monster(monsters[i]);
        uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
        spawn_monster_packet(monster->oid, monster->id, monster->x, monster->y, monster->fh, true, packet);
        room_broadcast(room, SPAWN_MONSTER_PACKET_LENGTH, packet);
    }

    if (controller != NULL) {
        for (size_t i = 0; i < count; i++) {
            struct IdMember *member = hash_set_u32_get(room->members, map_player_id(controller));
            const struct Monster *monster = map_monster_get_monster(monsters[i]);
            uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
            spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, true, packet);
            session_write(member->member->session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
        }
    }

    free(monsters);
}

static void wait_for_events(struct Worker *worker, void *);

void room_destroy(struct EventManager *mgr, struct Room *room)
{
    if (room != NULL) {
        map_destroy(room->map);
        room->map = NULL;
        event_unlisten(event_manager_get_event(mgr, EVENT_GLOBAL_RESPAWN), 0, room->respawnListener);
        // A callback could be in-flight right now so it's not enough to just stop listening,
        worker_command(room->worker, wait_for_events, room);
    }
}

static void wait_for_events(struct Worker *worker, void *user_data)
{
    struct Room *room = user_data;
    hash_set_u32_destroy(room->members);
    free(room);
}

static void do_broadcast_by(void *data_, void *ctx_);

void room_member_broadcast(struct Room *room, struct RoomMember *member, size_t len, uint8_t *packet)
{
    struct {
        struct RoomMember *member;
        size_t len;
        uint8_t *packet;
    } ctx = { member, len, packet };

    hash_set_u32_foreach(room->members, do_broadcast_by, &ctx);
}

static void do_broadcast_by(void *data_, void *ctx_)
{
    struct IdMember *data = data_;
    struct Session *session = data->member->session;
    struct {
        struct RoomMember *member;
        size_t len;
        uint8_t *packet;
    } *ctx = ctx_;

    if (session != ctx->member->session)
        session_write(session, ctx->len, ctx->packet);
}

static void do_broadcast(void *data_, void *ctx_);

static void room_broadcast(struct Room *room, size_t len, uint8_t *packet)
{
    struct iovec vec = { packet, len };

    hash_set_u32_foreach(room->members, do_broadcast, &vec);
}

static void do_broadcast(void *data_, void *ctx_)
{
    struct IdMember *data = data_;
    struct Session *session = data->member->session;
    struct iovec *vec = ctx_;

    session_write(session, vec->iov_len, vec->iov_base);
}

static void do_notify_drop(struct Drop *drop, void *ctx);
static void do_notify_monster(struct MapMonster *map_monster, void *ctx_);
static void do_notify_monster_controller(struct MapMonster *monster, void *ctx_);
static void do_notify_npc(struct Npc *npc, void *ctx);
static void do_notify_player(struct MapPlayer *player, void *ctx);

struct RoomMember *room_join(struct Room *room, struct Session *session, struct Player *player, struct HashSetU32 *quest_items, struct ScriptManager *reactor_manager)
{
    struct Map *map = room->map;
    struct RoomMember *member = malloc(sizeof(struct RoomMember));
    if (member == NULL) {
        return NULL;
    }

    struct IdMember id_member = { session_id(session), member };
    if (hash_set_u32_insert(room->members, &id_member) == -1) {
        free(member);
        return NULL;
    }

    member->player = map_join(map, session_id(session), player);
    if (member->player == NULL) {
        hash_set_u32_remove(room->members, session_id(session));
        free(member);
        return NULL;
    }

    member->visibleMapObjects = hash_set_u32_create(sizeof(uint32_t), 0);
    if (member->visibleMapObjects == NULL) {
        map_leave(map, member->player);
        hash_set_u32_remove(room->members, session_id(session));
        free(member);
        return NULL;
    }

    struct {
        struct Session *session;
        bool success;
    } ctx = { session, true };
    map_for_each_drop(map, do_notify_drop, &ctx);
    if (!ctx.success) {
        hash_set_u32_destroy(member->visibleMapObjects);
        map_leave(map, member->player);
        hash_set_u32_remove(room->members, session_id(session));
        free(member);
        return NULL;
    }

    map_for_each_monster(map, do_notify_monster, &ctx);
    if (!ctx.success) {
        hash_set_u32_destroy(member->visibleMapObjects);
        map_leave(map, member->player);
        hash_set_u32_remove(room->members, session_id(session));
        free(member);
        return NULL;
    }

    map_for_each_npc(map, do_notify_npc, &ctx);
    if (!ctx.success) {
        hash_set_u32_destroy(member->visibleMapObjects);
        map_leave(map, member->player);
        hash_set_u32_remove(room->members, session_id(session));
        free(member);
        return NULL;
    }

    if (hash_set_u32_size(room->members) == 1) {
        map_for_each_monster(map, do_notify_monster_controller, &ctx);
        if (!ctx.success) {
            hash_set_u32_destroy(member->visibleMapObjects);
            map_leave(map, member->player);
            hash_set_u32_remove(room->members, session_id(session));
            free(member);
            return NULL;
        }
    }

    {
        struct {
            struct Session *session;
            struct MapPlayer *player;
            bool success;
        } ctx = { session, member->player, true };
        map_for_each_player(map, do_notify_player, &ctx);
        if (!ctx.success) {
            hash_set_u32_destroy(member->visibleMapObjects);
            map_leave(map, member->player);
            hash_set_u32_remove(room->members, session_id(session));
            free(member);
            return NULL;
        }
    }

    member->room = room;
    member->session = session;
    member->questItems = quest_items;
    member->reactorManager = reactor_manager;

    uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
    size_t len = add_player_to_map_packet(map_player_get_player(member->player), packet);
    room_member_broadcast(room, member, len, packet);

    return member;
}

static void do_notify_drop(struct Drop *drop, void *ctx_)
{
    struct {
        struct Session *session;
        bool success;
    } *ctx = ctx_;

    if (!ctx->success)
        return;

    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[SPAWN_MESO_DROP_PACKET_LENGTH];
        spawn_meso_drop_packet(drop->oid, drop->meso, drop->qid, drop->type, drop->x, drop->y, false, packet);
        if (session_write(ctx->session, SPAWN_MONSTER_PACKET_LENGTH, packet) == -1)
            ctx->success = false;
    }
    break;
    }

}

static void do_notify_monster(struct MapMonster *map_monster, void *ctx_)
{
    struct {
        struct Session *session;
        bool success;
    } *ctx = ctx_;

    if (!ctx->success)
        return;

    const struct Monster *monster = map_monster_get_monster(map_monster);

    uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
    spawn_monster_packet(monster->oid, monster->id, monster->x, monster->y, monster->fh, false, packet);
    if (session_write(ctx->session, SPAWN_MONSTER_PACKET_LENGTH, packet) == -1)
        ctx->success = false;
}

static void do_notify_monster_controller(struct MapMonster *map_monster, void *ctx_)
{
    struct {
        struct Session *session;
        bool success;
    } *ctx = ctx_;

    if (!ctx->success)
        return;

    const struct Monster *monster = map_monster_get_monster(map_monster);
    uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
    spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
    if (session_write(ctx->session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet) == -1)
        ctx->success = false;
}

static void do_notify_npc(struct Npc *npc, void *ctx_)
{
    struct {
        struct Session *session;
        bool success;
    } *ctx = ctx_;

    if (!ctx->success)
        return;

    {
        uint8_t packet[SPAWN_NPC_PACKET_LENGTH];
        spawn_npc_packet(npc->oid, npc->id, npc->x, npc->cy, npc->f, npc->fh, npc->rx0, npc->rx1, packet);
        if (session_write(ctx->session, SPAWN_NPC_PACKET_LENGTH, packet) == -1) {
            ctx->success = false;
            return;
        }
    }

    {
        uint8_t packet[SPAWN_NPC_CONTROLLER_PACKET_LENGTH];
        spawn_npc_controller_packet(npc->oid, npc->id, npc->x, npc->cy, npc->f, npc->fh, npc->rx0, npc->rx1, packet);
        if (session_write(ctx->session, SPAWN_NPC_CONTROLLER_PACKET_LENGTH, packet) == -1)
            ctx->success = false;
    }
}

static void do_notify_player(struct MapPlayer *player, void *ctx_)
{
    struct {
        struct Session *session;
        struct MapPlayer *player;
        bool success;
    } *ctx = ctx_;

    if (!ctx->success)
        return;

    if (ctx->player != player) {
        uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
        size_t len = add_player_to_map_packet(map_player_get_player(player), packet);
        if (session_write(ctx->session, len, packet) == -1)
            ctx->success = false;
    }
}

static void do_notify_control(struct MapMonster *map_monster, void *ctx);

void room_leave(struct Room *room, struct RoomMember *member)
{
    if (member != NULL) {
        struct Map *map = room->map;
        struct MapPlayer *player = member->player;
        struct Session *session = member->session;
        struct MapPlayer *next = map_next_controller(map, player);
        if (next != NULL) {
            struct IdMember *id_member = hash_set_u32_get(room->members, session_id(session));
            assert(id_member != NULL);
            struct RoomMember *new = id_member->member;
            map_player_for_each_controlled_monster(player, do_notify_control, new);
        }

        map_leave(map, player);

        {
            uint8_t packet[REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH];
            remove_player_from_map_packet(session_id(session), packet);
            room_member_broadcast(room, member, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);
        }

        hash_set_u32_remove(room->members, session_id(member->session));
        hash_set_u32_destroy(member->visibleMapObjects);
    }

    free(member);
}

static void do_notify_control(struct MapMonster *map_monster, void *ctx)
{
    struct RoomMember *member = ctx;
    map_monster_swap_controller(map_monster, member->player);
    const struct Monster *monster = map_monster_get_monster(map_monster);

    uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
    spawn_monster_controller_packet(monster->oid, false, monster->id, monster->x, monster->y, monster->fh, false, packet);
    session_write(member->session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
}

uint32_t room_id(struct Room *room)
{
    return room->id;
}

bool room_keep_alive(struct Room *room)
{
    return hash_set_u32_size(room->members) > 0 || room->timerCount > 0;
}

static void do_foreach_member(void *data, void *ctx);

void room_foreach_member(struct Room *room, void (*f)(struct RoomMember *, void *), void *ctx_)
{
    struct {
        void (*f)(struct RoomMember *, void *);
        void *ctx;
    } ctx = { f, ctx_ };
    hash_set_u32_foreach(room->members, do_foreach_member, &ctx);
}

static void do_foreach_member(void *data, void *ctx_)
{
    struct IdMember *member_id = data;
    struct {
        void (*f)(struct RoomMember *, void *);
        void *ctx;
    } *ctx = ctx_;

    ctx->f(member_id->member, ctx->ctx);
}

void room_broadcast_stand_up(struct Room *room, uint16_t id)
{
    map_tire_seat(room->map, id);
}

bool room_monster_exists(struct Room *room, uint32_t oid, uint32_t id)
{
    struct Map *map = room->map;
    struct MapMonster *map_monster = map_get_monster(map, oid);
    if (map_monster == NULL)
        return false;

    return map_monster_get_monster(map_monster)->id == id;
}

uint32_t room_get_npc(struct Room *room, uint32_t oid)
{
    struct Map *map = room->map;
    return map_get_npc(map, oid);
}

bool room_member_close_range_attack(struct Room *room, struct RoomMember *member, uint8_t monster_count, uint8_t hit_count, uint32_t skill, uint8_t level, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t *oids, int32_t *damage, size_t *count, uint32_t *ids)
{
    monster_count = fixup_monster_oids(room, monster_count, hit_count, oids, damage);

    uint8_t packet[CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH];
    size_t len = close_range_attack_packet(session_id(member->session), skill, level, monster_count, hit_count, oids, damage, display, direction, stance, speed, packet);
    room_member_broadcast(room, member, len, packet);

    return damage_monsters(room, member, oids, monster_count, hit_count, damage, count, ids);
}

bool room_member_ranged_attack(struct Room *room, struct RoomMember *member, uint8_t monster_count, uint8_t hit_count, uint32_t skill, uint8_t level, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t *oids, int32_t *damage, uint32_t projectile, size_t *count, uint32_t *ids)
{
    monster_count = fixup_monster_oids(room, monster_count, hit_count, oids, damage);

    uint8_t packet[CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH];
    size_t len = ranged_attack_packet(session_id(member->session), skill, level, monster_count, hit_count, oids, damage, display, direction, stance, speed, projectile, packet);
    room_member_broadcast(room, member, len, packet);

    return damage_monsters(room, member, oids, monster_count, hit_count, damage, count, ids);
}

bool room_member_magic_attack(struct Room *room, struct RoomMember *member, uint8_t monster_count, uint8_t hit_count, uint32_t skill, uint8_t level, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t *oids, int32_t *damage, size_t *count, uint32_t *ids)
{
    monster_count = fixup_monster_oids(room, monster_count, hit_count, oids, damage);

    uint8_t packet[MAGIC_ATTACK_PACKET_MAX_LENGTH];
    size_t len = magic_attack_packet(session_id(member->session), skill, level, monster_count, hit_count, oids, damage, display, direction, stance, speed, packet);
    room_member_broadcast(room, member, len, packet);

    return damage_monsters(room, member, oids, monster_count, hit_count, damage, count, ids);
}

void room_member_update_stance(struct RoomMember *member, uint8_t stance)
{
    struct MapPlayer *player = member->player;
    map_player_update_stance(player, stance);
}

void room_member_update_coords(struct RoomMember *member, int16_t x, int16_t y, uint16_t fh)
{
    struct MapPlayer *player = member->player;
    map_player_update_pos(player, x, y, fh);
}

void room_member_move(struct Room *room, struct RoomMember *member, size_t len, uint8_t *packet)
{
    struct Session *session = member->session;

    uint8_t out[MOVE_PLAYER_PACKET_MAX_LENGTH];
    size_t out_len = move_player_packet(session_id(session), len, packet, out);
    room_member_broadcast(room, member, out_len, out);
}

static void do_announce_drop_batch(struct Drop *drop, void *ctx);

static bool damage_monsters(struct Room *room, struct RoomMember *member, uint32_t *oids, uint8_t monster_count, uint8_t hit_count, int32_t *damage, size_t *count, uint32_t *ids)
{
    struct Map *map = room->map;
    struct Session *session = member->session;

    *count = 0;
    for (size_t i = 0; i < monster_count; i++) {
        struct MapMonster *map_monster = map_get_monster(map, oids[i]);
        const struct Monster *monster = map_monster_get_monster(map_monster);
        if (map_monster_damage_by(map_monster, member->player, hit_count, damage)) {
            struct MapPlayer *old = map_monster_swap_controller(map_monster, member->player);
            if (old != NULL) {
                //struct RoomMember *old_member = (void *)map_player_ctx(old);
                struct IdMember *old_member = hash_set_u32_get(room->members, map_player_id(old));
                uint8_t packet[REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH];
                remove_monster_controller_packet(oids[i], packet);
                session_write(old_member->member->session, REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH, packet);
            }

            uint8_t packet[SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH];
            spawn_monster_controller_packet(oids[i], false, monster->id, monster->x, monster->y, monster->fh, false, packet);
            if (session_write(session, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet) == -1)
                return false;
        }

        uint8_t packet[MONSTER_HP_PACKET_LENGTH];
        monster_hp_packet(oids[i], monster->hp * 100 / wz_get_monster_stats(monster->id)->hp, packet);
        session_write(session, MONSTER_HP_PACKET_LENGTH, packet);

        if (monster->hp == 0) {
            const struct MonsterDropInfo *info = drop_info_find(monster->id);

            // TODO: Abstract away the loot construction

            // Monster that don't have any drops return NULL
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
                            for (int32_t j = 0; j < drops_copy[i].chance / 1000000; j++) {
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
                            for (int32_t j = 0; j < drops_copy[i].chance / 1000000; j++) {
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

                struct DropBatch *batch = map_add_drop_batch(room->map, member->player, true, monster->oid, (struct Point) { monster->x, monster->y }, drop_count, drops);
                struct {
                    struct Room *room;
                    struct RoomMember *member;
                    uint32_t owner;
                    int16_t x, y;
                } ctx = { room, member, session_id(session), monster->x, monster->y };
                drop_batch_for_each_drop(batch, do_announce_drop_batch, &ctx);
            }

            ids[i] = monster->id;
            (*count)++;
            {
                uint8_t packet[KILL_MONSTER_PACKET_LENGTH];
                kill_monster_packet(monster->oid, true, packet);
                room_broadcast(room, KILL_MONSTER_PACKET_LENGTH, packet);
            }
            map_remove_monster(map, map_monster);
            return true;
        }
    }

    return false;
}

static void do_announce_drop(struct RoomMember *member, void *ctx);
static void add_visible_item(struct RoomMember *, void *);

static void do_announce_drop_batch(struct Drop *drop, void *ctx_)
{
    struct {
        struct Room *room;
        struct RoomMember *member;
        uint32_t owner;
        int16_t x, y;
    } *ctx = ctx_;
    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
        drop_meso_from_object_packet(drop->oid, drop->meso, ctx->owner, 1, ctx->x, ctx->y, drop->x, drop->y, drop->oid, false, packet);
        room_member_broadcast(ctx->room, ctx->member, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
        drop_meso_from_object_packet(drop->oid, drop->meso, ctx->owner, 2, ctx->x, ctx->y, drop->x, drop->y, drop->oid, false, packet);
        session_write(ctx->member->session, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
        room_foreach_member(ctx->room, add_visible_item, &drop->oid);
    }
    break;

    case DROP_TYPE_ITEM: {
        struct {
            struct Drop *drop;
            struct RoomMember *member;
            uint32_t owner;
            int16_t x, y;
        } subctx = { drop, ctx->member, ctx->owner, ctx->x, ctx->y };
        room_foreach_member(ctx->room, do_announce_drop, &subctx);
    }
    break;

    case DROP_TYPE_EQUIP: {
        // As far as I know, no equip item is quest-exclusive
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, ctx->owner, 1, ctx->x, ctx->y, drop->x, drop->y, drop->oid, false, packet);
        room_member_broadcast(ctx->room, ctx->member, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        room_foreach_member(ctx->room, add_visible_item, &drop->oid);
        drop_meso_from_object_packet(drop->oid, drop->equip.item.itemId, ctx->owner, 2, ctx->x, ctx->y, drop->x, drop->y, drop->oid, false, packet);
        session_write(ctx->member->session, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
        room_foreach_member(ctx->room, add_visible_item, &drop->oid);
    }
    break;
    }
}

static void do_announce_drop(struct RoomMember *member, void *ctx_)
{
    struct {
        struct Drop *drop;
        struct RoomMember *member;
        uint32_t owner;
        int16_t x, y;
    } *ctx = ctx_;
    uint32_t id = ctx->drop->item.item.itemId;
    if (!wz_get_item_info(id)->quest || hash_set_u32_get(member->questItems, id) != NULL) {
        struct Drop *drop = ctx->drop;
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, id, ctx->owner, session_id(member->session) == ctx->owner ? 2 : 1,
                ctx->x, ctx->y, drop->x, drop->y, ctx->drop->oid, false, packet);
        session_write(member->session, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        hash_set_u32_insert(member->visibleMapObjects, &drop->oid);
    }
}

static void add_visible_item(struct RoomMember *member, void *ctx)
{
    hash_set_u32_insert(member->visibleMapObjects, ctx);
}

int room_member_sit_packet(struct Room *room, struct RoomMember *member, uint16_t id)
{
    struct Session *session = member->session;
    if (id == (uint16_t)-1) {
        if (!map_player_is_seated(member->player)) {
            session_shutdown(session);
            return -1;
        }

        uint16_t seat = map_player_get_map_seat(member->player);
        map_player_stand_up(member->player);

        // If we were sitting on our own chair, make sure to set the chair to 0
        if (seat == (uint16_t)-1) {
            uint8_t packet[SET_CHAIR_PACKET_LENGTH];
            set_chair_packet(map_player_id(member->player), 0, packet);
            room_member_broadcast(room, member, SET_CHAIR_PACKET_LENGTH, packet);
        }

        uint8_t packet[STAND_UP_PACKET_LENGTH];
        stand_up_packet(packet);
        return session_write(session, STAND_UP_PACKET_LENGTH, packet);
    } else {
        if (id >= wz_get_map_seat_count(room->id)) {
            session_shutdown(session);
            return -1;
        }

        if (map_player_get_map_seat(member->player) != (uint16_t)-1)
            return 0;

        if (!map_try_occupy_seat(room->map, id))
            return 0;

        map_player_sit(member->player, id);

        uint8_t packet[SIT_ON_MAP_SEAT_PACKET_LENGTH];
        sit_on_map_seat_packet(id, packet);
        return session_write(session, SIT_ON_MAP_SEAT_PACKET_LENGTH, packet);

        // Other map players will be notified of the sitting by a move packet
    }

    return 0;
}

int room_member_chair(struct Room *room, struct RoomMember *member, uint32_t id)
{
    if (map_player_is_seated(member->player))
        return 0;

    map_player_chair(member->player, id);

    uint8_t packet[SET_CHAIR_PACKET_LENGTH];
    set_chair_packet(session_id(member->session), id, packet);
    room_member_broadcast(room, member, SET_CHAIR_PACKET_LENGTH, packet);

    return 0;
}

void room_member_pick_up_drop(struct Room *room, struct RoomMember *member, uint32_t oid)
{
    struct Session *session = member->session;
    //struct Map *map = room->map;
    map_remove_drop(room->map, oid);
    uint8_t packet[PICKUP_DROP_PACKET_LENGTH];
    pickup_drop_packet(oid, false, session_id(session), packet);
    room_broadcast(room, PICKUP_DROP_PACKET_LENGTH, packet);
}

void room_member_level_up(struct Room *room, struct RoomMember *member)
{
    struct Session *session = member->session;
    uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
    show_foreign_effect_packet(session_id(session), 0, packet);
    room_member_broadcast(room, member, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
}

void room_member_effect(struct Room *room, struct RoomMember *member, uint8_t effect)
{
    struct Session *session = member->session;
    uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
    show_foreign_effect_packet(session_id(session), effect, packet);
    room_member_broadcast(room, member, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
}

void room_member_take_damage(struct Room *room, struct RoomMember *member, uint8_t skill, int32_t damage, uint32_t id, uint8_t direction)
{
    struct Session *session = member->session;
    uint8_t packet[DAMAGE_PLAYER_PACKET_MAX_LENGTH];
    size_t len = damange_player_packet(session_id(session), skill, damage, id, direction, 0, packet);
    room_member_broadcast(room, member, len, packet);
}

void room_member_chat(struct Room *room, struct RoomMember *member, size_t len, char *string, uint8_t show)
{
    struct Session *session = member->session;
    uint8_t packet[CHAT_PACKET_MAX_LENGTH];
    len = chat_packet(session_id(session), false, len, string, show, packet);
    room_broadcast(room, len, packet);
}

void room_member_emote(struct Room *room, struct RoomMember *member, uint32_t emote)
{
    struct Session *session = member->session;
    uint8_t packet[FACE_EXPRESSION_PACKET_LENGTH];
    face_expression_packet(session_id(session), emote, packet);
    room_member_broadcast(room, member, FACE_EXPRESSION_PACKET_LENGTH, packet);
}

static void do_room_notify_drop(struct Drop *drop, void *ctx);

bool room_member_drop(struct Room *room, struct RoomMember *member, struct Drop *drop)
{
    struct Map *map = room->map;
    struct MapPlayer *player = member->player;
    struct DropBatch *batch = map_add_drop_batch(map, member->player, map_player_id(player), false, map_player_coords(player), 1, drop);
    if (batch == NULL)
        return false;

    struct {
        struct Room *room;
        uint32_t id;
        struct Point origin;
    } ctx;
    drop_batch_for_each_drop(batch, do_room_notify_drop, &ctx);
    return true;
}

static void do_room_notify_drop(struct Drop *drop, void *ctx_)
{
    struct {
        struct Room *room;
        uint32_t id;
        struct Point origin;
    } *ctx = ctx_;
    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
        drop_meso_from_object_packet(drop->oid, drop->meso, ctx->id, 2, ctx->origin.x, ctx->origin.y, drop->x, drop->y, ctx->id, true, packet);
        room_broadcast(ctx->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_ITEM: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->item.item.itemId, ctx->id, 2, ctx->origin.x, ctx->origin.y, drop->x, drop->y, ctx->id, true, packet);
        room_broadcast(ctx->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_EQUIP: {
        uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
        drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, ctx->id, 2, ctx->origin.x, ctx->origin.y, drop->x, drop->y, ctx->id, true, packet);
        room_broadcast(ctx->room, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;
    }

    struct {
        uint32_t oid;
    } ctx1 = { drop->oid };
    room_foreach_member(ctx->room, add_visible_item, &ctx1);
}

bool room_member_add_quest_items(struct RoomMember *member, size_t count, uint32_t *ids)
{
    struct Session *session = member->session;

    for (size_t i = 0; i < count; i++) {
        if (hash_set_u32_insert(member->questItems, &ids[i]) == -1) {
            session_shutdown(session);
            return false;
        }
    }

    return true;
}

bool room_member_move_monster(struct Room *room, struct RoomMember *member, uint32_t oid, uint16_t moveid, uint8_t activity, uint8_t skill_id, uint8_t skill_level, uint16_t option, size_t len, uint8_t *packet, int16_t x, int16_t y, uint16_t fh, uint8_t stance)
{
    struct Map *map = room->map;
    struct Session *session = member->session;
    struct MapPlayer *player = member->player;
    if (!map_move_monster(map, player, oid, x, y, fh, stance))
        return false;

    {
        uint8_t send[MOVE_MONSTER_PACKET_MAX_LENGTH];
        size_t packet_len = move_monster_packet(oid, true, activity, skill_id, skill_level, option, len, packet, send);
        room_member_broadcast(room, member, packet_len, send);
    }

    {
        uint8_t packet[MOVE_MONSTER_RESPONSE_PACKET_LENGTH];
        move_monster_response_packet(oid, moveid, packet);
        session_write(session, MOVE_MONSTER_RESPONSE_PACKET_LENGTH, packet);
    }

    return true;
}

bool room_member_get_drop(struct Room *room, struct RoomMember *member, uint32_t oid, struct Drop *drop)
{
    struct Map *map = room->map;
    if (hash_set_u32_get(member->visibleMapObjects, oid) == NULL)
        return false;

    const struct Drop *drop_ = map_get_drop(map, oid);
    if (drop == NULL)
        return false;

    *drop = *drop_;

    return true;
}

void room_member_hit_reactor(struct Room *room, struct RoomMember *member, uint32_t oid, uint8_t stance)
{
    struct Map *map = room->map;
    struct Session *session = member->session;
    struct MapPlayer *player = member->player;

    uint32_t id;
    const char *script_name = map_hit_reactor(map, player, oid, &id);
    if (script_name != NULL) {
        // Reactor broken
        char script_name[37];
        strcpy(script_name, script_name);
        strcat(script_name, ".lua");
        member->script = script_manager_alloc(member->reactorManager, script_name, 0);
        if (member->script == NULL) {
            return;
        }

        member->rm = reactor_manager_create(room, member, oid, id);

        enum ScriptResult res = script_manager_run(member->script, SCRIPT_REACTOR_MANAGER_TYPE, member->rm);

        switch (res) {
        case SCRIPT_RESULT_VALUE_KICK:
        case SCRIPT_RESULT_VALUE_FAILURE:
        case SCRIPT_RESULT_VALUE_SUCCESS:
            reactor_manager_destroy(member->rm);
            script_manager_free(member->script);
            member->script = NULL;
            if (res != SCRIPT_RESULT_VALUE_SUCCESS)
                session_shutdown(session);
            return;
        break;
        case SCRIPT_RESULT_VALUE_NEXT:
            return;
        break;
        }
    } else {
        // Reactor got hit
        const struct Reactor *reactor = map_get_reactor(map, oid);
        uint8_t packet[CHANGE_REACTOR_STATE_PACKET_LENGTH];
        change_reactor_state_packet(oid, reactor->state, reactor->x, reactor->y, stance, packet);
        room_broadcast(room, CHANGE_REACTOR_STATE_PACKET_LENGTH, packet);
    }
}

static size_t fixup_monster_oids(struct Room *room, size_t monster_count, size_t hit_count, uint32_t *oids, int32_t *damage)
{
    for (size_t i = 0; i < monster_count; i++) {
        if (map_get_monster(room->map, oids[i]) == NULL) {
            oids[i] = oids[monster_count - 1];
            memcpy(damage + i * hit_count, damage + (monster_count - 1) * hit_count, hit_count * sizeof(int32_t));
            i--;
            monster_count--;
        }
    }

    return monster_count;
}


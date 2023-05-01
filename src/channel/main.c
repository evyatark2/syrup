//#define _POSIX_C_SOURCE // For strtok_r
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "client.h"
#include "drops.h"
#include "events.h"
#include "server.h"
#include "shop.h"
#include "../constants.h"
#include "../reader.h"
#include "../hash-map.h"
#include "map.h"
#include "../packet.h"

#define ACCOUNT_MAX_NAME_LENGTH 12
#define ACCOUNT_MAX_PASSWORD_LENGTH 12
#define ACCOUNT_HWID_LENGTH 10

struct GlobalContext {
    struct ScriptManager *questManager;
    struct ScriptManager *portalManager;
    struct ScriptManager *npcManager;
    struct ScriptManager *mapManager;
    struct ScriptManager *reactorManager;
};

static void on_log(enum LogType type, const char *fmt, ...);

static void *create_context(void);
static void destroy_context(void *ctx);
static void on_client_connect(struct Session *session, void *global_ctx, void *thread_ctx, struct sockaddr *addr);
static void on_client_disconnect(struct Session *session);
static void on_client_join(struct Session *session, void *thread_ctx);
static void on_client_packet(struct Session *session, size_t size, uint8_t *packet);
static void on_unassigned_client_packet(struct Session *session, size_t size, uint8_t *packet);
static void on_client_resume(struct Session *session, int fd, int status);
static int on_room_create(struct Room *room, void *thread_ctx);
static void on_room_destroy(struct Room *room);

static void on_sigint(int sig);

struct ChannelServer *SERVER;

int main(void)
{
    if (channel_config_load("channel/config.json") == -1)
        return -1;
    const char *ip;
    const char *socket;
    struct in_addr addr4;
    struct in6_addr addr6;
    if (inet_pton(AF_INET, CHANNEL_CONFIG.database.host, &addr4) == 1 || inet_pton(AF_INET6, CHANNEL_CONFIG.database.host, &addr6) == 1) {
        ip = CHANNEL_CONFIG.database.host;
        socket = NULL;
    } else {
        ip = NULL;
        socket = CHANNEL_CONFIG.database.host;
    }

    struct DatabaseConnection *conn = database_connection_create(ip, CHANNEL_CONFIG.database.user, CHANNEL_CONFIG.database.password, CHANNEL_CONFIG.database.db, CHANNEL_CONFIG.database.port, socket);
    if (conn == NULL) {
        channel_config_unload();
        return -1;
    }
    int ret = drops_load_from_db(conn);
    if (ret == -1) {
        channel_config_unload();
        return -1;
    }

    ret = shops_load_from_db(conn);
    database_connection_destroy(conn);
    if (ret == -1) {
        drops_unload();
        channel_config_unload();
        return -1;
    }

    if (wz_init() != 0) {
        shops_unload();
        drops_unload();
        channel_config_unload();
        return -1;
    }

    struct GlobalContext ctx;

    enum ScriptValueType arg1 = SCRIPT_VALUE_TYPE_USERDATA;
    enum ScriptValueType arg2 = SCRIPT_VALUE_TYPE_USERDATA;
    struct ScriptEntryPoint eps[] = {
        {
            .name = "start",
            .argCount = 1,
            .args = &arg1,
        },
        {
            .name = "end_",
            .argCount = 1,
            .args = &arg2,
        }
    };

    SERVER = channel_server_create(7575, on_log, CHANNEL_CONFIG.listen, create_context, destroy_context, on_client_connect, on_client_disconnect, on_client_join, on_unassigned_client_packet, on_client_packet, on_room_create, on_room_destroy, &ctx, 7);
    if (SERVER == NULL)
        return -1;

    ctx.questManager = script_manager_create(SERVER, "script/quest", "def.lua", 2, eps);
    if (ctx.questManager == NULL) {
        wz_terminate();
        return -1;
    }

    eps->name = "enter";
    ctx.portalManager = script_manager_create(SERVER, "script/portal", "def.lua", 1, eps);
    if (ctx.portalManager == NULL) {
        script_manager_destroy(ctx.questManager);
        wz_terminate();
        return -1;
    }

    ctx.mapManager = script_manager_create(SERVER, "script/map/onUserEnter", "def.lua", 1, eps);
    if (ctx.mapManager == NULL) {
        script_manager_destroy(ctx.portalManager);
        script_manager_destroy(ctx.questManager);
        wz_terminate();
        return -1;
    }

    eps->name = "talk";
    ctx.npcManager = script_manager_create(SERVER, "script/npc", "def.lua", 1, eps);
    if (ctx.npcManager == NULL) {
        script_manager_destroy(ctx.mapManager);
        script_manager_destroy(ctx.portalManager);
        script_manager_destroy(ctx.questManager);
        wz_terminate();
        return -1;
    }

    eps->name = "act";
    ctx.reactorManager = script_manager_create(SERVER, "script/reactor", "def.lua", 1, eps);
    if (ctx.reactorManager == NULL) {
        script_manager_destroy(ctx.npcManager);
        script_manager_destroy(ctx.mapManager);
        script_manager_destroy(ctx.portalManager);
        script_manager_destroy(ctx.questManager);
        wz_terminate();
        return -1;
    }

    event_boat_init(SERVER);
    event_train_init(SERVER);
    event_genie_init(SERVER);
    event_subway_init(SERVER);
    event_area_boss_init(SERVER);

    signal(SIGINT, on_sigint);
    channel_server_start(SERVER);
    channel_server_destroy(SERVER);
    script_manager_destroy(ctx.reactorManager);
    script_manager_destroy(ctx.mapManager);
    script_manager_destroy(ctx.npcManager);
    script_manager_destroy(ctx.portalManager);
    script_manager_destroy(ctx.questManager);
    wz_terminate();
    shops_unload();
    drops_unload();
}

static void on_log(enum LogType type, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static void *create_context(void)
{
    const char *ip;
    const char *socket;
    struct in_addr addr4;
    struct in6_addr addr6;
    if (inet_pton(AF_INET, CHANNEL_CONFIG.database.host, &addr4) == 1 || inet_pton(AF_INET6, CHANNEL_CONFIG.database.host, &addr6) == 1) {
        ip = CHANNEL_CONFIG.database.host;
        socket = NULL;
    } else {
        ip = NULL;
        socket = CHANNEL_CONFIG.database.host;
    }

    return database_connection_create(ip, CHANNEL_CONFIG.database.user, CHANNEL_CONFIG.database.password, CHANNEL_CONFIG.database.db, CHANNEL_CONFIG.database.port, socket);
}

static void destroy_context(void *ctx)
{
    database_connection_destroy(ctx);
}

static void on_client_connect(struct Session *session, void *global_ctx, void *thread_ctx, struct sockaddr *addr)
{
    struct GlobalContext *ctx = global_ctx;
    if (!session_accept(session))
        return;

    struct Client *client = client_create(session, thread_ctx, ctx->questManager, ctx->portalManager, ctx->npcManager, ctx->mapManager);
    if (client == NULL)
        session_kick(session);

    session_set_context(session, client);
}

#define READER_BEGIN(size, packet) { \
        struct Reader reader__; \
        reader_init(&reader__, (size), (packet));

#define SKIP(size) \
        reader_skip(&reader__, (size))

#define READ_OR_ERROR(func, ...) \
        if (!func(&reader__, ##__VA_ARGS__)) \
            session_kick(session); \

#define READER_AVAILABLE() \
        (reader__.size - reader__.pos)

#define READER_END() \
    }


static void on_client_packet(struct Session *session, size_t size, uint8_t *packet)
{
    if (size < 2)
        session_kick(session);

    struct Client *client = session_get_context(session);
    const struct Character *chr = client_get_character(client);
    uint16_t opcode;
    memcpy(&opcode, packet, sizeof(uint16_t));

    packet += 2;
    size -= 2;
    switch (opcode) {
    case 0x0026: {
        uint32_t target;
        uint16_t len = PORTAL_INFO_NAME_MAX_LENGTH;
        char portal[PORTAL_INFO_NAME_MAX_LENGTH+1];
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_ERROR(reader_u32, &target);
        READ_OR_ERROR(reader_sized_string, &len, portal);
        SKIP(1);
        SKIP(2); // wheel
        READER_END();
        if (target == -1) {
            portal[len] = '\0';
            uint32_t target_map = wz_get_target_map(chr->map, portal);
            if (target_map == -1)
                session_kick(session);
            uint8_t target_portal = wz_get_target_portal(chr->map, portal);
            if (target_portal == (uint8_t)-1)
                return;
            client_warp(client, target_map, target_portal);
        } else {
            if (chr->hp > 0) {
                if (chr->map == 0 || chr->map / 1000 == 1020) {
                    uint32_t id = wz_get_map_forced_return(chr->map);
                    if (id != target)
                        session_kick(session);

                    const struct PortalInfo *portal = wz_get_portal_info_by_name(id, "sp");

                    client_warp(client, id, portal->id);
                }
            } else {
                uint32_t id = wz_get_map_nearest_town(chr->map);
                const struct PortalInfo *portal = wz_get_portal_info_by_name(id, "sp");

                client_set_hp_now(client, 50);

                client_warp(client, id, portal->id);
            }
        }
    }
    break;

    case 0x0029: {
        if (client_get_map(client)->player == NULL)
            return;

        size_t len = 1;
        uint8_t count;
        READER_BEGIN(size, packet);
        SKIP(9);
        READ_OR_ERROR(reader_u8, &count);
        for (uint8_t i = 0; i < count; i++) {
            uint8_t command;
            READ_OR_ERROR(reader_u8, &command);
            len++;
            switch (command) {
            case 0:
            case 5:
            case 17: {
                int16_t x;
                int16_t y;
                uint16_t fh;
                uint8_t stance;
                //SKIP(13);
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
                SKIP(4); // x y wobble
                READ_OR_ERROR(reader_u16, &fh);
                READ_OR_ERROR(reader_u8, &stance);
                SKIP(2); // duration
                len += 13;
                client_update_player_pos(client, x, y, fh, stance);
            }
            break;

            case 1:
            case 2:
            case 6:
            case 12:
            case 13:
            case 16:
            case 18:
            case 19:
            case 20:
            case 22: {
                uint8_t stance;
                SKIP(4); // Relative movement
                READ_OR_ERROR(reader_u8, &stance);
                SKIP(2); // duration
                client_update_player_pos(client, chr->x, chr->y, chr->fh, stance);
                len += 7;
            }
            break;

            case 3:
            case 4:
            case 7:
            case 8:
            case 9:
            case 11: {
                uint8_t stance;
                SKIP(8); // Relative movement, with wobble
                READ_OR_ERROR(reader_u8, &stance);
                client_update_player_pos(client, chr->x, chr->y, chr->fh, stance);
                len += 9;
            }
            break;

            case 10: // Change equip
                SKIP(1);
                len += 1;
            break;

            case 14:
                SKIP(9);
                len += 9;
            break;

            case 15: {
                uint8_t stance;
                SKIP(12); // x, y, and wobbles, fh, origin fh
                READ_OR_ERROR(reader_u8, &stance);
                SKIP(2); // duration
                client_update_player_pos(client, chr->x, chr->y, chr->fh, stance);
                len += 15;
            }
            break;

            //case 21:
            //    SKIP(3);
            //    len += 3;
            //break;
            }
        }
        SKIP(18);
        READER_END();

        {
            uint8_t out[MOVE_PLAYER_PACKET_MAX_LENGTH];
            size_t out_len = move_player_packet(chr->id, len, packet + 9, out);
            session_broadcast_to_room(session, out_len, out);
        }
    }
    break;

    case 0x002A: {
        uint16_t id;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u16, &id);
        READER_END();

        if (id == (uint16_t)-1) {
            if (!client_stand_up(client))
                session_kick(session);
        } else {
            if (!client_sit_on_map_seat(client, id))
                session_kick(session);
        }
    }
    break;

    case 0x002B: {
        uint32_t id;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u32, &id);
        READER_END();

        if (!client_sit(client, id))
            session_kick(session);
    }
    break;

    case 0x002C: {
        struct Map *map = room_get_context(session_get_room(session));
        uint32_t oids[15];
        int32_t damage[15 * 15];
        uint8_t monster_count;
        uint8_t hit_count;
        uint32_t skill;
        uint8_t display;
        uint8_t direction;
        uint8_t stance;
        uint8_t speed;
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &hit_count);
        monster_count = hit_count >> 4;
        hit_count &= 0xF;
        READ_OR_ERROR(reader_u32, &skill);
        SKIP(8);
        READ_OR_ERROR(reader_u8, &display);
        READ_OR_ERROR(reader_u8, &direction);
        READ_OR_ERROR(reader_u8, &stance);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &speed);
        SKIP(4);
        for (int8_t i = 0; i < monster_count; i++) {
            READ_OR_ERROR(reader_u32, &oids[i]);
            SKIP(14);

            for (int8_t j = 0; j < hit_count; j++)
                READ_OR_ERROR(reader_i32, &damage[i * hit_count + j]);


            SKIP(4);
        }
        SKIP(4);
        READER_END();

        uint8_t skill_level = 0;
        if (skill != 0) {
            if (!client_apply_skill(client, skill, &skill_level))
                session_kick(session);
        }

        {
            uint8_t packet[CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH];
            size_t len = close_range_attack_packet(chr->id, skill, skill_level, monster_count, hit_count, oids, damage, display, direction, stance, speed, packet);
            session_broadcast_to_room(session, len, packet);
        }

        for (uint8_t i = 0; i < monster_count; i++) {
            uint32_t killed = map_damage_monster_by(map, client_get_map(client)->player, chr->id, oids[i], hit_count, damage + i * hit_count);
            if (killed != -1)
                client_kill_monster(client, killed);
        }
    }
    break;

    case 0x002D: {
        struct Map *map = room_get_context(session_get_room(session));
        uint32_t oids[15];
        int32_t damage[15 * 15];
        uint8_t monster_count;
        uint8_t hit_count;
        uint32_t skill;
        uint8_t display;
        uint8_t direction;
        uint8_t stance;
        uint8_t speed;
        uint8_t ranged_direction;
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &hit_count);
        monster_count = hit_count >> 4;
        hit_count &= 0xF;
        READ_OR_ERROR(reader_u32, &skill);
        SKIP(8);
        READ_OR_ERROR(reader_u8, &display);
        READ_OR_ERROR(reader_u8, &direction);
        READ_OR_ERROR(reader_u8, &stance);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &speed);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &ranged_direction);
        SKIP(7);
        for (int8_t i = 0; i < monster_count; i++) {
            READ_OR_ERROR(reader_u32, &oids[i]);
            SKIP(14);

            for (int8_t j = 0; j < hit_count; j++)
                READ_OR_ERROR(reader_i32, &damage[i * hit_count + j]);


            SKIP(4);
        }
        SKIP(4);
        READER_END();

        uint8_t skill_level = 0;
        if (skill == 0) {
            bool success;
            client_use_projectile(client, 1, &success);
        } else {
            if (!client_apply_skill(client, skill, &skill_level))
                session_kick(session);
        }

        {
            uint8_t packet[RANGED_ATTACK_PACKET_MAX_LENGTH];
            size_t len = ranged_attack_packet(chr->id, skill, skill_level, monster_count, hit_count, oids, damage, display, direction, stance, speed, chr->inventory[0].items[chr->activeProjectile].item.item.itemId, packet);
            session_broadcast_to_room(session, len, packet);
        }

        for (uint8_t i = 0; i < monster_count; i++) {
            uint32_t killed = map_damage_monster_by(map, client_get_map(client)->player, chr->id, oids[i], hit_count, damage + i * hit_count);
            if (killed != -1)
                client_kill_monster(client, killed);
        }
    }
    break;

    case 0x002E: {
        struct Map *map = room_get_context(session_get_room(session));
        uint32_t oids[15];
        int32_t damage[15 * 15];
        uint8_t monster_count;
        uint8_t hit_count;
        uint32_t skill;
        uint8_t display;
        uint8_t direction;
        uint8_t stance;
        uint8_t speed;
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &hit_count);
        monster_count = hit_count >> 4;
        hit_count &= 0xF;
        READ_OR_ERROR(reader_u32, &skill);
        SKIP(8);
        READ_OR_ERROR(reader_u8, &display);
        READ_OR_ERROR(reader_u8, &direction);
        READ_OR_ERROR(reader_u8, &stance);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &speed);
        SKIP(4);
        for (int8_t i = 0; i < monster_count; i++) {
            READ_OR_ERROR(reader_u32, &oids[i]);
            SKIP(14);

            for (int8_t j = 0; j < hit_count; j++)
                READ_OR_ERROR(reader_i32, &damage[i * hit_count + j]);


            SKIP(4);
        }
        SKIP(4);
        READER_END();

        uint8_t skill_level = 0;
        if (skill != 0) {
            if (!client_apply_skill(client, skill, &skill_level))
                session_kick(session);
        }

        {
            uint8_t packet[MAGIC_ATTACK_PACKET_MAX_LENGTH];
            size_t len = magic_attack_packet(chr->id, skill, skill_level, monster_count, hit_count, oids, damage, display, direction, stance, speed, packet);
            session_broadcast_to_room(session, len, packet);
        }

        for (uint8_t i = 0; i < monster_count; i++) {
            uint32_t killed = map_damage_monster_by(map, client_get_map(client)->player, chr->id, oids[i], hit_count, damage + i * hit_count);
            if (killed != -1)
                client_kill_monster(client, killed);
        }

    }
    break;

    case 0x0030: {
        struct Map *map = room_get_context(session_get_room(session));
        uint8_t skill;
        int32_t damage;
        uint32_t monster_id;
        uint32_t oid;
        uint8_t direction;
        READER_BEGIN(size, packet);
        SKIP(4);
        READ_OR_ERROR(reader_u8, &skill);
        SKIP(1); // Element
        READ_OR_ERROR(reader_i32, &damage);
        if (skill != (uint8_t)-3 && skill != (uint8_t)-4) {
            READ_OR_ERROR(reader_u32, &monster_id);
            READ_OR_ERROR(reader_u32, &oid);
            READ_OR_ERROR(reader_u8, &direction);
        }

        READER_END();

        if (skill != (uint8_t)-3 && skill != (uint8_t)-4) {
            if (map_monster_is_alive(map, monster_id, oid)) {
                client_adjust_hp(client, -damage);
                client_commit_stats(client);
                uint8_t packet[DAMAGE_PLAYER_PACKET_MAX_LENGTH];
                size_t len = damange_player_packet(skill, monster_id, chr->id, damage, 0, direction, packet);
                session_broadcast_to_room(session, len, packet);
            }
        }
    }
    break;

    case 0x0031: {
        uint16_t str_len = 80;
        char string[81];
        uint8_t show;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_sized_string, &str_len, string);
        if (string[0] != '/')
            READ_OR_ERROR(reader_u8, &show);
        READER_END();

        if (string[0] == '!') {
            if (!strncmp(string + 1, "autopickup", str_len - 1)) {
                client_toggle_auto_pickup(client);
                if (client_is_auto_pickup_enabled(client)) {
                    map_pick_up_all(room_get_context(session_get_room(session)), client_get_map(client)->player);
                }
            } else if (!strncmp(string + 1, "killall", str_len - 1)) {
                size_t count;
                uint32_t *ids = map_kill_all_by(room_get_context(session_get_room(session)), client_get_map(client)->player, &count);
                for (size_t i = 0; i < count; i++)
                    client_kill_monster(client, ids[i]);
                free(ids);
            } else {
                // strtok() must take a nul-terminated string
                string[str_len] = '\0';
                char *save;
                char *token = strtok_r(string + 1, " ", &save);
                if (!strcmp(token, "map")) {
                    token = strtok_r(NULL, " ", &save);
                    const struct MapInfo *info = wz_get_map(strtol(token, NULL, 10));
                    if (info != NULL) {
                        client_warp(client, info->id, 0);
                    } else {
                        client_message(client, "Requested map doesn't exist");
                    }
                }
            }
        } else if (string[0] != '/') {
            uint8_t packet[CHAT_PACKET_MAX_LENGTH];
            size_t len = chat_packet(chr->id, false, str_len, string, show, packet);
            room_broadcast(session_get_room(session), len, packet);
        }
    }
    break;

    case 0x0033: {
        uint32_t emote;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u32, &emote);
        READER_END();

        // TODO: Check emote legality
        {
            uint8_t packet[FACE_EXPRESSION_PACKET_LENGTH];
            face_expression_packet(chr->id, emote, packet);
            session_broadcast_to_room(session, FACE_EXPRESSION_PACKET_LENGTH, packet);
        }
    }
    break;

    case 0x003A: {
        if (client_get_map(client)->player == NULL)
            session_kick(session);

        struct Map *map = room_get_context(session_get_room(session));
        uint32_t oid;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u32, &oid);
        READER_END();

        uint32_t id = map_get_npc(map, oid);
        if (id == -1)
            session_kick(session);

        struct ClientResult res = client_npc_talk(client, id);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_BAN:
            case CLIENT_RESULT_TYPE_ERROR:
                session_kick(session);
            case CLIENT_RESULT_TYPE_SUCCESS:
            case CLIENT_RESULT_TYPE_NEXT:
                return;
            }
    }
    break;

    case 0x003C: {
        uint8_t last;
        uint8_t action;
        uint32_t selection = -1;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u8, &last);
        READ_OR_ERROR(reader_u8, &action);
        if (last == NPC_DIALOGUE_TYPE_SIMPLE || last == 3) {
            if (READER_AVAILABLE() >= 4) {
                READ_OR_ERROR(reader_u32, &selection);
            } else if (READER_AVAILABLE() > 0) {
                uint8_t sel_u8;
                READ_OR_ERROR(reader_u8, &sel_u8);
                selection = sel_u8;
            }
        }

        READER_END();

        uint32_t action_u32 = last == NPC_DIALOGUE_TYPE_SIMPLE || last == 3 ?
            (action == 1 ? selection : (action == 0 ? -1 : action)) :
            (action == (uint8_t)-1 ? -1 : action);
        struct ClientResult res = client_script_cont(client, action_u32);
        switch (res.type) {
        case CLIENT_RESULT_TYPE_BAN:
        case CLIENT_RESULT_TYPE_ERROR:
            session_kick(session);
        case CLIENT_RESULT_TYPE_SUCCESS:
        case CLIENT_RESULT_TYPE_NEXT:
            return;
        }
    }
    break;

    case 0x003D: {
        uint8_t action;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u8, &action);
        switch (action) {
        case 0: { // Buy
            uint16_t slot;
            uint32_t id;
            int16_t quantity;
            int32_t price;
            READ_OR_ERROR(reader_u16, &slot);
            READ_OR_ERROR(reader_u32, &id);
            READ_OR_ERROR(reader_i16, &quantity);
            READ_OR_ERROR(reader_i32, &price);
            struct ClientResult res = client_buy(client, slot, id, quantity, price);
            if (res.type < 0)
                session_kick(session);
        }
        break;

        case 1: { // Sell
            uint16_t slot;
            uint32_t id;
            int16_t quantity;
            READ_OR_ERROR(reader_u16, &slot);
            READ_OR_ERROR(reader_u32, &id);
            READ_OR_ERROR(reader_i16, &quantity);
            struct ClientResult res = client_sell(client, slot, id, quantity);
            if (res.type < 0)
                session_kick(session);
        }
        break;

        case 2: { // Recharge
            uint16_t slot;
            READ_OR_ERROR(reader_u16, &slot);
            struct ClientResult res = client_recharge(client, slot);
            if (res.type < 0)
                session_kick(session);
        }
        break;

        case 3: // Leave
            if (!client_close_shop(client))
                session_kick(session);
        break;

        }
        READER_END();
    }
    break;

    case 0x0047: {
        struct Map *map = room_get_context(session_get_room(session));

        if (client_is_in_shop(client))
            return;

        uint8_t inventory;
        int16_t src;
        int16_t dst;
        int16_t quantity;
        READER_BEGIN(size, packet);
        SKIP(4);
        READ_OR_ERROR(reader_u8, &inventory);
        READ_OR_ERROR(reader_i16, &src);
        READ_OR_ERROR(reader_i16, &dst);
        READ_OR_ERROR(reader_i16, &quantity);
        READER_END();

        if (dst == 0) {
            if (quantity <= 0)
                session_kick(session);

            struct Drop drop;

            drop.x = chr->x;
            drop.y = chr->y;
            if (inventory != 1) {
                drop.type = DROP_TYPE_ITEM;
                bool success;
                if (!client_remove_item(client, inventory, src, quantity, &success, &drop.item))
                    session_kick(session);

                if (!success)
                    return;
            } else {
                if (quantity > 1)
                    session_kick(session);

                drop.type = DROP_TYPE_EQUIP;
                if (src < 0) {
                    bool success;
                    if (!client_remove_equip(client, true, -src, &success, &drop.equip))
                        session_kick(session);

                    if (!success)
                        return;
                } else {
                    bool success;
                    if (!client_remove_equip(client, false, src, &success, &drop.equip))
                        session_kick(session);

                    if (!success)
                        return;
                }
            }

            map_add_player_drop(map, client_get_map(client)->player, &drop);
        } else {
            if (quantity != -1)
                session_kick(session);

            if (inventory != 1 || (src > 0 && dst > 0)) {
                if (!client_move_item(client, inventory, src, dst))
                    session_kick(session);
            } else {
                if (dst < 0) {
                    if (!client_equip(client, src, -dst))
                        session_kick(session);
                } else {
                    if (!client_unequip(client, -src, dst))
                        session_kick(session);
                }
            }
        }
    }
    break;

    case 0x0048: {
        uint16_t slot;
        uint32_t item_id;
        READER_BEGIN(size, packet);
        SKIP(4);
        READ_OR_ERROR(reader_u16, &slot);
        READ_OR_ERROR(reader_u32, &item_id);
        READER_END();

        if (!client_use_item(client, slot, item_id))
            session_kick(session);
    }
    break;

    case 0x0057: {
        uint32_t stat;
        READER_BEGIN(size, packet);
        SKIP(4);
        READ_OR_ERROR(reader_u32, &stat);
        READER_END();

        switch (stat) {
        case 0x40:
            client_adjust_str(client, 1);
        break;
        case 0x80:
            client_adjust_dex(client, 1);
        break;
        case 0x100:
            client_adjust_int(client, 1);
        break;
        case 0x200:
            client_adjust_luk(client, 1);
        break;
        default:
            session_kick(session);
        }

        client_commit_stats(client);
    }
    break;

    // 59 00 70 FC 02 00 00 14 00 00 00 00 03 00 00
    case 0x0059: {
        int16_t hp, mp;
        READER_BEGIN(size, packet);
        SKIP(8);
        READ_OR_ERROR(reader_i16, &hp);
        READ_OR_ERROR(reader_i16, &mp);
        SKIP(1);
        READER_END();

        // TODO: Check if the client is allowed to heal this much HP/MP
        client_adjust_hp(client, hp);
        client_adjust_mp(client, mp);
        client_commit_stats(client);
    }
    break;

    case 0x005A: {
        uint32_t id;
        READER_BEGIN(size, packet);
        SKIP(4);
        READ_OR_ERROR(reader_u32, &id);
        READER_END();

        if (!client_assign_sp(client, id))
            session_kick(session);
    }
    break;

    case 0x005E: {
        int32_t amount;
        READER_BEGIN(size, packet);
        SKIP(4);
        READ_OR_ERROR(reader_i32, &amount);
        READER_END();

        if (amount < 10 || amount > 50000)
            session_kick(session);

        if (chr->mesos < amount)
            return;

        client_gain_meso(client, -amount, false, false);
        client_commit_stats(client);

        struct Drop drop = {
            .type = DROP_TYPE_MESO,
            .x = chr->x,
            .y = chr->y,
            .meso = amount
        };
        map_add_player_drop(room_get_context(session_get_room(session)), client_get_map(client)->player, &drop);
    }
    break;

    case 0x0064: {
        uint16_t len = 17;
        char str[17];
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_ERROR(reader_sized_string, &len, str);
        SKIP(4);
        READER_END();

        str[len] = '\0';
        const struct PortalInfo *info = wz_get_portal_info_by_name(chr->map, str);
        if (info == NULL) {
            // The client can spam enter portal which will cause a search for the portal in the destination map
            return;
        }
        struct ClientResult res = client_launch_portal_script(client, info->script);
        switch (res.type) {
        case CLIENT_RESULT_TYPE_BAN:
        case CLIENT_RESULT_TYPE_ERROR:
            session_kick(session);
        case CLIENT_RESULT_TYPE_SUCCESS:
        case CLIENT_RESULT_TYPE_NEXT:
            return;
        break;
        }
    }
    break;

    case 0x006B: {
        uint8_t action;
        uint16_t qid;
        uint32_t npc;
        int16_t x, y;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u8, &action);
        READ_OR_ERROR(reader_u16, &qid);
        switch (action) {
        case 0: {
            uint32_t item_id;
            SKIP(4);
            READ_OR_ERROR(reader_u32, &item_id);
            struct ClientResult res = client_regain_quest_item(client, qid, item_id);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_BAN:
            case CLIENT_RESULT_TYPE_ERROR:
                session_kick(session);
            case CLIENT_RESULT_TYPE_SUCCESS:
            case CLIENT_RESULT_TYPE_NEXT:
                return;
            }
        }
        break;

        case 1: {
            READ_OR_ERROR(reader_u32, &npc);
            if (READER_AVAILABLE() == 4) {
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
            }
            struct ClientResult res = client_start_quest(client, qid, npc, false);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_BAN:
            case CLIENT_RESULT_TYPE_ERROR:
                session_kick(session);
            case CLIENT_RESULT_TYPE_SUCCESS:
            case CLIENT_RESULT_TYPE_NEXT:
                return;
            }
        }
        break;

        case 2: {
            READ_OR_ERROR(reader_u32, &npc);
            if (READER_AVAILABLE() == 4) {
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
            }
            //uint8_t selection;
            struct ClientResult res = client_end_quest(client, qid, npc, false);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_BAN:
            case CLIENT_RESULT_TYPE_ERROR:
                session_kick(session);
            case CLIENT_RESULT_TYPE_SUCCESS:
            case CLIENT_RESULT_TYPE_NEXT:
                return;
            }
        }
        break;

        case 3: {
            if (!client_forfeit_quest(client, qid))
                session_kick(session);
        }
        break;

        case 4: {
            READ_OR_ERROR(reader_u32, &npc);
            if (READER_AVAILABLE() == 4) {
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
            }
            struct ClientResult res = client_start_quest(client, qid, npc, true);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_BAN:
            case CLIENT_RESULT_TYPE_ERROR:
                session_kick(session);
            case CLIENT_RESULT_TYPE_SUCCESS:
            case CLIENT_RESULT_TYPE_NEXT:
                return;
            }
        }
        break;

        case 5: {
            READ_OR_ERROR(reader_u32, &npc);
            if (READER_AVAILABLE() == 4) {
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
            }
            struct ClientResult res = client_end_quest(client, qid, npc, true);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_BAN:
            case CLIENT_RESULT_TYPE_ERROR:
                session_kick(session);
            case CLIENT_RESULT_TYPE_SUCCESS:
            case CLIENT_RESULT_TYPE_NEXT:
                return;
            }
        }
        break;

        default:
            session_kick(session);
        }
        READER_END();
    }
    break;

    case 0x0087: {
        if (client_get_map(client)->player == NULL)
            return;

        uint32_t mode;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u32, &mode);
        if (mode == 0) {
            uint32_t changeCount;
            READ_OR_ERROR(reader_u32, &changeCount);
            for (size_t i = 0; i < changeCount; i++) {
                uint32_t key;
                uint8_t type;
                uint32_t action;
                READ_OR_ERROR(reader_u32, &key);
                READ_OR_ERROR(reader_u8, &type);
                READ_OR_ERROR(reader_u32, &action);
                if (type > 0) {
                    if (type == 1) {
                        if (!client_add_skill_key(client, key, action))
                            return;
                    } else {
                        if (!client_add_key(client, key, type, action))
                            return;
                    }
                } else {
                    if (!client_remove_key(client, key, action))
                        return;
                }
            }
        } else if (mode == 1) { // Auto-HP
        } else if (mode == 2) { // Auto-MP
        } else {
            // Illegal mode
            return;
        }
        READER_END();
    }
    break;

    case 0x00B7: {
    }
    break;

    case 0x00BC: {
        if (client_get_map(client)->player == NULL)
            return;

        struct Map *map = room_get_context(session_get_room(session));
        uint32_t oid;
        uint16_t moveid;
        uint8_t activity;
        int16_t x, y;
        uint16_t fh;
        uint8_t stance;
        size_t len = 5; // startx, starty, count
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u32, &oid);
        READ_OR_ERROR(reader_u16, &moveid);
        SKIP(1);
        READ_OR_ERROR(reader_u8, &activity);
        SKIP(21);
        uint8_t count;
        READ_OR_ERROR(reader_u8, &count);
        for (uint8_t i = 0; i < count; i++) {
            uint8_t command;
            READ_OR_ERROR(reader_u8, &command);
            len++;
            switch (command) {
            case 0:
            case 5:
            case 17:
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
                SKIP(4);
                READ_OR_ERROR(reader_u16, &fh);
                READ_OR_ERROR(reader_u8, &stance);

                SKIP(2);
                len += 13;
            break;

            case 1:
            case 2:
            case 6:
            case 12:
            case 13:
            case 16:
            case 18:
            case 19:
            case 20:
            case 22:
                SKIP(7);
                len += 7;
            break;

            case 3:
            case 4:
            case 7:
            case 8:
            case 9:
            case 11:
            case 14:
                SKIP(9);
                len += 9;
            break;

            case 10:
                SKIP(1);
                len += 1;
            break;

            case 15:
                SKIP(15);
                len += 15;
            break;

            case 21:
                SKIP(3);
                len += 3;
            break;
            }
        }
        SKIP(9);
        READER_END();

        if (map_move_monster(map, client_get_map(client)->player, activity, oid, x, y, fh, stance, len, packet + 25)) {
            {
                uint8_t send[MOVE_MONSTER_PACKET_MAX_LENGTH];
                size_t packet_len = move_monster_packet(oid, -1, len, packet + 25, send);
                session_broadcast_to_room(session, packet_len, send);
            }

            {
                uint8_t packet[MOVE_MONSTER_RESPONSE_PACKET_LENGTH];
                move_monster_response_packet(oid, moveid, packet);
                session_write(session, MOVE_MONSTER_RESPONSE_PACKET_LENGTH, packet);
            }
        }
    }
    break;

    case 0x00C5: {
        if (client_get_map(client)->player == NULL)
            return;

        READER_BEGIN(size, packet);
        if (size == 6) {
            uint8_t data[6];
            uint8_t out[8];
            READ_OR_ERROR(reader_array, 6, data);
            npc_action_packet(6, data, out);
            session_write(session, 8, out);
        } else if (size > 9) {
            uint8_t data[size - 9];
            uint8_t out[size - 7];
            READ_OR_ERROR(reader_array, size - 9, data);
            npc_action_packet(size - 9, data, out);
            session_write(session, size - 7, out);
        } else {
            session_kick(session);
        }

        READER_END();
    }
    break;

    case 0x00CA: {
        uint32_t oid;
        READER_BEGIN(size, packet);
        SKIP(9);
        READ_OR_ERROR(reader_u32, &oid);
        READER_END();
        const struct Drop *drop = map_get_drop(room_get_context(session_get_room(session)), oid);
        if (drop == NULL) {
            {
                uint8_t packet[ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH];
                item_unavailable_notification_packet(packet);
                session_write(session, ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH, packet);
            }
            {
                uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                size_t len = stat_change_packet(true, 0, NULL, packet);
                session_write(session, len, packet);
            }
        } else {
            if (!map_player_can_pick_up_drop(room_get_context(session_get_room(session)), client_get_map(client)->player, oid)) {
                uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                size_t len = stat_change_packet(true, 0, NULL, packet);
                session_write(session, len, packet);
                return;
            }

            enum InventoryGainResult result = false;
            uint32_t id;
            switch (drop->type) {
            case DROP_TYPE_MESO:
                client_gain_meso(client, drop->meso, true, false);
                client_commit_stats(client);
                map_remove_drop(room_get_context(session_get_room(session)), chr->id, oid);
                return;
            break;

            case DROP_TYPE_ITEM:
                if (drop->item.item.itemId / 1000000 == 2 && wz_get_consumable_info(drop->item.item.itemId)->consumeOnPickup) {
                    client_use_item_immediate(client, drop->item.item.itemId);
                    map_remove_drop(room_get_context(session_get_room(session)), chr->id, oid);
                    return;
                } else if (!client_gain_inventory_item(client, &drop->item, &result)) {
                    session_kick(session);
                }

                id = drop->item.item.itemId;
            break;

            case DROP_TYPE_EQUIP:
                if (!client_gain_equipment(client, &drop->equip, false, &result))
                    session_kick(session);

                id = drop->equip.item.itemId;
            break;
            }

            if (result == INVENTORY_GAIN_RESULT_SUCCESS) {
                {
                    uint8_t packet[ITEM_GAIN_PACKET_LENGTH];
                    item_gain_packet(id, 1, packet);
                    session_write(session, ITEM_GAIN_PACKET_LENGTH, packet);
                }

                map_remove_drop(room_get_context(session_get_room(session)), chr->id, oid);

                {
                    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                    size_t len = stat_change_packet(true, 0, NULL, packet);
                    session_write(session, len, packet);
                }
            } else if (result == INVENTORY_GAIN_RESULT_FULL) {
                {
                    uint8_t packet[INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH];
                    inventory_full_notification_packet(packet);
                    session_write(session, INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH, packet);
                }

                {
                    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
                    size_t len = modify_items_packet(0, NULL, packet);
                    session_write(session, len, packet);
                }
            } else if (result == INVENTORY_GAIN_RESULT_UNAVAILABLE) {
                {
                    uint8_t packet[ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH];
                    item_unavailable_notification_packet(packet);
                    session_write(session, ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH, packet);
                }

                {
                    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                    size_t len = stat_change_packet(true, 0, NULL, packet);
                    session_write(session, len, packet);
                }
            }
        }
    }
    break;

    case 0x00CD: {
        uint32_t oid;
        uint8_t stance;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u32, &oid);
        SKIP(4); // Position
        READ_OR_ERROR(reader_u8, &stance);
        SKIP(5);
        SKIP(4); // skill ID
        READER_END();

        struct ClientResult res = map_hit_reactor(room_get_context(session_get_room(session)), client_get_map(client)->player, oid, stance);
        switch (res.type) {
        case CLIENT_RESULT_TYPE_BAN:
        case CLIENT_RESULT_TYPE_ERROR:
            session_kick(session);
        case CLIENT_RESULT_TYPE_SUCCESS:
        case CLIENT_RESULT_TYPE_NEXT:
            return;
        }
    }
    break;

    case 0x00CF: {
        READER_BEGIN(size, packet);
        READER_END();
        struct Map *map = room_get_context(session_get_room(session));
        session_enable_write(session);

        if (map_join(map, client, client_get_map(client)) == -2)
            return;

        // Forced stat reset
        session_write(session, 2, (uint8_t[]) { 0x23, 0x00 }); // Forced stat reset

        struct ClientResult res = client_script_cont(client, 0);
        if (res.type == CLIENT_RESULT_TYPE_BAN || res.type == CLIENT_RESULT_TYPE_ERROR)
            session_kick(session);

        if (res.type != CLIENT_RESULT_TYPE_NEXT && wz_get_map_enter_script(chr->map) != NULL) {
            struct ClientResult res = client_launch_map_script(client, wz_get_map_enter_script(chr->map));
            switch (res.type) {
            case CLIENT_RESULT_TYPE_BAN:
            case CLIENT_RESULT_TYPE_ERROR:
                session_kick(session);
            case CLIENT_RESULT_TYPE_SUCCESS:
            case CLIENT_RESULT_TYPE_NEXT:
                return;
            }
        } else if (res.type == CLIENT_RESULT_TYPE_NEXT) {
            // TODO: Queue up the map's script
        }
    }
    break;

    default:
    break;
    }
}

static void on_unassigned_client_packet(struct Session *session, size_t size, uint8_t *packet)
{
    if (size < 2)
        session_kick(session);

    struct Client *client = session_get_context(session);
    const struct Character *chr = client_get_character(client);
    uint16_t opcode;
    memcpy(&opcode, packet, sizeof(uint16_t));

    if (opcode != 0x0014)
        session_kick(session);

    uint32_t token;
    READER_BEGIN(size - 2, packet + 2);
    READ_OR_ERROR(reader_u32, &token);
    SKIP(2);
    READER_END();
    uint32_t id;
    if (!session_assign_token(session, token, &id))
        session_kick(session);

    //const struct sockaddr *addr = session_get_addr(session);

    //struct IdAddr new = {
    //    .id = id,
    //};

    //memcpy(&new.addr,
    //        addr->sa_family == AF_INET ? (void *)(struct sockaddr_in *)addr : (void *)(struct sockaddr_in6 *)addr,
    //        addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));

    //hash_set_u32_insert(CLIENTS, &new);

    client_login_start(client, id);
    struct ClientContResult res = client_cont(client, 0);

    if (res.status > 0) {
        session_set_event(session, res.status, res.fd, on_client_resume);
        return;
    } else if (res.status == 0) {
        session_change_room(session, chr->map);
    }
}

static void on_client_resume(struct Session *session, int fd, int status)
{
    struct Client *client = session_get_context(session);
    const struct Character *chr = client_get_character(client);

    struct ClientContResult res = client_cont(client, status);
    if (res.status > 0 && (res.fd != session_get_event_fd(session) || res.status != session_get_event_disposition(session))) {
        session_set_event(session, res.status, res.fd, on_client_resume);
        return;
    } else if (res.status == 0) {
        session_close_event(session);
        session_change_room(session, chr->map);
    }
}

static void on_client_resume_disconnect(struct Session *session, int fd, int status)
{
    struct Client *client = session_get_context(session);

    struct ClientContResult res = client_cont(client, status);
    if (res.status > 0 && (res.fd != session_get_event_fd(session) || res.status != session_get_event_disposition(session))) {
        session_set_event(session, res.status, res.fd, on_client_resume_disconnect);
    } else if (res.status <= 0) {
        session_close_event(session);
    }
}

static void on_client_disconnect(struct Session *session)
{
    struct Client *client = session_get_context(session);
    const struct Character *chr = client_get_character(client);
    if (session_get_room(session) != NULL) {
        uint8_t packet[REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH];
        remove_player_from_map_packet(chr->id, packet);
        session_broadcast_to_room(session, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);
    }

    client_logout_start(client);
    struct ClientContResult res = client_cont(client, 0);
    if (res.status > 0) {
        session_set_event(session, res.status, res.fd, on_client_resume_disconnect);
    }
}

static void on_client_join(struct Session *session, void *thread_ctx)
{
    struct Client *client = session_get_context(session);
    const struct Character *chr = client_get_character(client);

    client_update_conn(client, thread_ctx);
    //session_write(session, 3, (uint8_t[]) { 0xDE, 0x00, 0x00 }); // Disable UI
    //session_write(session, 3, (uint8_t[]) { 0xDD, 0x00, 0x00 }); // Lock UI
    //session_write(session, 2, (uint8_t[]) { 0x23, 0x00 }); // Force stat reset
    const struct PortalInfo *info = wz_get_portal_info(chr->map, chr->spawnPoint);
    client_update_player_pos(client, info->x, info->y, 0, 6);
}

static int on_room_create(struct Room *room, void *thread_ctx)
{
    struct GlobalContext *ctx = thread_ctx;

    struct Map *map = map_create(SERVER, room, ctx->reactorManager);
    if (map == NULL)
        return -1;

    room_set_context(room, map);

    return 0;
}

static void on_room_destroy(struct Room *room)
{
    map_destroy(room_get_context(room));
}

static void on_sigint(int sig)
{
    channel_server_stop(SERVER);
    signal(sig, SIG_DFL);
}


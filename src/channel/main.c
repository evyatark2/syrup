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
#include "server.h"
#include "handlers.h"
#include "../constants.h"
#include "../reader.h"
#include "../hash-map.h"
#include "map.h"
#include "scripting/script-manager.h"

#define ACCOUNT_MAX_NAME_LENGTH 12
#define ACCOUNT_MAX_PASSWORD_LENGTH 12
#define ACCOUNT_HWID_LENGTH 10

struct GlobalContext {
    struct ScriptManager *questManager;
    struct ScriptManager *portalManager;
    struct ScriptManager *npcManager;
};

static void on_log(enum LogType type, const char *fmt, ...);

static void *create_context();
static void destroy_context(void *ctx);
static int on_client_connect(struct Session *session, void *global_ctx, void *thread_ctx, struct sockaddr *addr);
static void on_client_disconnect(struct Session *session);
static struct OnResumeResult on_resume_client_disconnect(struct Session *session, int fd, int status);
static bool on_client_join(struct Session *session, void *thread_ctx);
static struct OnPacketResult on_client_packet(struct Session *session, size_t size, uint8_t *packet);
static struct OnPacketResult on_unassigned_client_packet(struct Session *session, size_t size, uint8_t *packet);
static struct OnResumeResult on_resume_client_packet(struct Session *session, int fd, int status);
static int on_room_create(struct Room *room, void *thread_ctx);
static void on_room_destroy(struct Room *room);

static struct OnResumeResult on_database_lock_ready(struct Session *session, int fd, int status);

static void on_sigint(int sig);

static void notify_player_on_map(struct Session *src, struct Session *dst, void *ctx);
static void notify_npc_on_map(struct Npc *npc, void *ctx);
static void notify_drop_on_map(struct Drop *drop, void *ctx);

struct ChannelServer *SERVER;

int main()
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
    database_connection_destroy(conn);
    if (ret == -1)
        return -1;

    if (wz_init() != 0)
        return -1;

    struct GlobalContext ctx;

    enum ScriptValueType arg1 = SCRIPT_VALUE_TYPE_USERDATA;
    enum ScriptValueType arg2 = SCRIPT_VALUE_TYPE_USERDATA;
    struct ScriptEntryPoint eps[] = {
        {
            .name = "start",
            .argCount = 1,
            .args = &arg1,
            .result = SCRIPT_VALUE_TYPE_VOID
        },
        {
            .name = "end_",
            .argCount = 1,
            .args = &arg2,
            .result = SCRIPT_VALUE_TYPE_VOID
        }
    };

    ctx.questManager = script_manager_create("script/quest", "def.lua", 2, eps);
    if (ctx.questManager == NULL) {
        wz_terminate();
        return -1;
    }

    eps->name = "enter";
    ctx.portalManager = script_manager_create("script/portal", "def.lua", 1, eps);
    if (ctx.portalManager == NULL) {
        script_manager_destroy(ctx.questManager);
        wz_terminate();
        return -1;
    }

    eps->name = "talk";
    ctx.npcManager = script_manager_create("script/npc", "def.lua", 1, eps);
    if (ctx.npcManager == NULL) {
        script_manager_destroy(ctx.portalManager);
        script_manager_destroy(ctx.questManager);
        wz_terminate();
        return -1;
    }

    SERVER = channel_server_create(7575, on_log, CHANNEL_CONFIG.listen, create_context, destroy_context, on_client_connect, on_client_disconnect, on_client_join, on_unassigned_client_packet, on_client_packet, on_room_create, on_room_destroy, &ctx);
    if (SERVER == NULL)
        return -1;

    signal(SIGINT, on_sigint);
    channel_server_start(SERVER);
    channel_server_destroy(SERVER);
    script_manager_destroy(ctx.npcManager);
    script_manager_destroy(ctx.portalManager);
    script_manager_destroy(ctx.questManager);
    wz_terminate();
    drops_unload();
}

static void on_log(enum LogType type, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static void *create_context(void *global_ctx)
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

static int on_client_connect(struct Session *session, void *global_ctx, void *thread_ctx, struct sockaddr *addr)
{
    struct GlobalContext *ctx = global_ctx;
    struct Client *client = malloc(sizeof(struct Client));
    if (client == NULL)
        return -1;

    client->session = session;
    client->conn = thread_ctx;
    client->map.handle = NULL;
    client->managers.quest = ctx->questManager;
    client->managers.portal = ctx->portalManager;
    client->managers.npc = ctx->npcManager;
    client->script = NULL;
    client->assigned = false;
    session_set_context(session, client);

    return 0;
}

#define READER_BEGIN(size, packet) { \
        struct Reader reader__; \
        reader_init(&reader__, (size), (packet));

#define SKIP(size) \
        reader_skip(&reader__, (size))

#define READ_OR_ERROR(func, ...) \
        if (!func(&reader__, ##__VA_ARGS__)) \
            return (struct OnPacketResult) { .status = -1 };

#define READER_AVAILABLE() \
        (reader__.size - reader__.pos)

#define READER_END() \
    }


static struct OnPacketResult on_client_packet(struct Session *session, size_t size, uint8_t *packet)
{
    if (size < 2)
        return (struct OnPacketResult) { .status = -1 };

    struct Client *client = session_get_context(session);
    uint16_t opcode;
    memcpy(&opcode, packet, sizeof(uint16_t));

    printf("Got packet with opcode %hu\n", opcode);
    for (size_t i = 0; i < size; i++) {
        printf("%02X ", packet[i]);
    }
    printf("\n\n");

    packet += 2;
    size -= 2;
    switch (opcode) {
    case 0x0026: {
        uint32_t id;
        uint16_t len = PORTAL_INFO_NAME_MAX_LENGTH;
        char portal[PORTAL_INFO_NAME_MAX_LENGTH+1];
        READER_BEGIN(size, packet);
        SKIP(1); // 0 - from dying; 1 - regular portal
        READ_OR_ERROR(reader_u32, &id);
        READ_OR_ERROR(reader_sized_string, &len, portal);
        SKIP(7);
        READER_END();
        portal[len] = '\0';
        uint32_t targetMap = wz_get_target_map(client->character.map, portal);
        if (targetMap == -1)
            return (struct OnPacketResult) { .status = 0, .room = -1 };
        uint8_t targetPortal = wz_get_target_portal(client->character.map, portal);
        if (targetPortal == (uint8_t)-1)
            return (struct OnPacketResult) { .status = 0, .room = -1 };
        client_warp(client, targetMap, targetPortal);
        return (struct OnPacketResult) { .status = 0, .room = client->targetMap };
    }
    break;

    case 0x0029: {
        if (client->map.handle == NULL)
            return (struct OnPacketResult) { .status = 0, .room = -1 };

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
            case 17:
                //SKIP(13);
                READ_OR_ERROR(reader_i16, &client->character.x);
                READ_OR_ERROR(reader_i16, &client->character.y);
                SKIP(4); // x y wobble
                READ_OR_ERROR(reader_u16, &client->character.fh);
                READ_OR_ERROR(reader_u8, &client->character.stance);
                SKIP(2); // duration
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
                SKIP(4); // Relative movement
                READ_OR_ERROR(reader_u8, &client->character.stance);
                SKIP(2); // duration
                len += 7;
            break;

            case 3:
            case 4:
            case 7:
            case 8:
            case 9:
            case 11:
                SKIP(8); // Relative movement, with wobble
                READ_OR_ERROR(reader_u8, &client->character.stance);
                len += 9;
            break;

            case 10: // Change equip
                SKIP(1);
                len += 1;
            break;

            case 14:
                SKIP(9);
                len += 9;
            break;

            case 15:
                SKIP(12); // x, y, and wobbles, fh, origin fh
                READ_OR_ERROR(reader_u8, &client->character.stance);
                SKIP(2); // duration
                len += 15;
            break;

            //case 21:
            //    SKIP(3);
            //    len += 3;
            //break;
            }
        }
        SKIP(18);
        READER_END();
        struct MovePlayerResult res = handle_move_player(client, len, packet + 9);
        session_broadcast_to_room(session, res.size, res.packet);
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

        {
            uint8_t packet[CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH];
            size_t len = close_range_attack_packet(client->character.id, skill, 0, monster_count, hit_count, oids, damage, display, direction, stance, speed, packet);
            session_broadcast_to_room(session, len, packet);
        }

        for (uint8_t i = 0; i < monster_count; i++) {
            uint32_t killed = map_damage_monster_by(map, client->map.handle, client->character.id, oids[i], hit_count, damage + i * hit_count);
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

        uint32_t projectile = 0;

        {
            uint8_t packet[CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH];
            size_t len = ranged_attack_packet(client->character.id, skill, 0, monster_count, hit_count, oids, damage, display, direction, stance, speed, projectile, packet);
            session_broadcast_to_room(session, len, packet);
        }

        for (uint8_t i = 0; i < monster_count; i++) {
            uint32_t killed = map_damage_monster_by(map, client->map.handle, client->character.id, oids[i], hit_count, damage + i * hit_count);
            if (killed != -1)
                client_kill_monster(client, killed);
        }
    }
    break;

    case 0x003A: {
        struct Map *map = room_get_context(session_get_room(session));
        uint32_t oid;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u32, &oid);
        READER_END();

        const struct Npc *npc = map_get_npc(map, oid);
        if (npc == NULL)
            return (struct OnPacketResult) { .status = -1 };

        struct ClientResult res = client_npc_talk(client, npc->id);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_KICK:
            case CLIENT_RESULT_TYPE_ERROR:
                return (struct OnPacketResult) { .status = -1 };
            case CLIENT_RESULT_TYPE_SUCCESS:
                return (struct OnPacketResult) { .status = 0, .room = -1 };
            case CLIENT_RESULT_TYPE_WARP:
                return (struct OnPacketResult) { .status = 0, .room = res.map };
            }
    }
    break;

    case 0x003C: {
        uint8_t last;
        uint8_t action;
        READER_BEGIN(size, packet);
        READ_OR_ERROR(reader_u8, &last);
        READ_OR_ERROR(reader_u8, &action);
        READER_END();
        if (client->script != NULL) {
            struct ClientResult res = client_script_cont(client, action);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_KICK:
            case CLIENT_RESULT_TYPE_ERROR:
                return (struct OnPacketResult) { .status = -1 };
            case CLIENT_RESULT_TYPE_SUCCESS:
                return (struct OnPacketResult) { .status = 0, .room = -1 };
            case CLIENT_RESULT_TYPE_WARP:
                return (struct OnPacketResult) { .status = 0, .room = res.map };
            }
        }
    }
    break;

    case 0x0047: {
        struct Map *map = room_get_context(session_get_room(session));

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
                return (struct OnPacketResult) { .status = -1 };

            struct Drop drop;

            drop.pos.x = client->character.x;
            drop.pos.y = client->character.y;
            if (inventory != 1) {
                drop.type = DROP_TYPE_ITEM;
                bool success;
                if (!client_remove_item(client, inventory, src, quantity, &success, &drop.item))
                    return (struct OnPacketResult) { .status = -1 };

                if (!success)
                    return (struct OnPacketResult) { .status = 0, .room = -1 };
            } else {
                if (quantity > 1)
                    return (struct OnPacketResult) { .status = -1 };

                drop.type = DROP_TYPE_EQUIP;
                if (src < 0) {
                    bool success;
                    if (!client_remove_equip(client, true, -src, &success, &drop.equip))
                        return (struct OnPacketResult) { .status = -1 };

                    if (!success)
                        return (struct OnPacketResult) { .status = 0, .room = -1 };
                } else {
                    bool success;
                    if (!client_remove_equip(client, false, src, &success, &drop.equip))
                        return (struct OnPacketResult) { .status = -1 };

                    if (!success)
                        return (struct OnPacketResult) { .status = 0, .room = -1 };
                }
            }

            map_add_drop_batch(map, client->character.id, -1, 1, &drop);
        } else {
            if (quantity != -1)
                return (struct OnPacketResult) { .status = -1 };

            if (inventory != 1 || (src > 0 && dst > 0)) {
                if (!client_move_item(client, inventory, src, dst))
                    return (struct OnPacketResult) { .status = -1 };
            } else {
                if (dst < 0) {
                    if (!client_equip(client, src, -dst))
                        return (struct OnPacketResult) { .status = -1 };
                } else {
                    if (!client_unequip(client, -src, dst))
                        return (struct OnPacketResult) { .status = -1 };
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
            return (struct OnPacketResult) { .status = -1 };
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
            client_raise_str(client);
        break;
        case 0x80:
            client_raise_dex(client);
        break;
        case 0x100:
            client_raise_int(client);
        break;
        case 0x200:
            client_raise_luk(client);
        break;
        default:
            return (struct OnPacketResult) { .status = -1 };
        }

        return (struct OnPacketResult) { .status = 0, .room = -1 };
    }
    break;

    case 0x0064: {
        if (client->script != NULL)
            return (struct OnPacketResult) { .status = 0, .room = -1 };

        uint16_t len = 17;
        char str[17];
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_ERROR(reader_sized_string, &len, str);
        SKIP(4);
        READER_END();

        str[len] = '\0';
        const struct PortalInfo *info = wz_get_portal_info_by_name(client->character.map, str);
        if (info == NULL)
            return (struct OnPacketResult) { .status = -1 };
        struct ClientResult res = client_portal_script(client, info->script);
        switch (res.type) {
        case CLIENT_RESULT_TYPE_KICK:
        case CLIENT_RESULT_TYPE_ERROR:
            return (struct OnPacketResult) { .status = -1 };
        case CLIENT_RESULT_TYPE_SUCCESS:
            return (struct OnPacketResult) { .status = 0, .room = -1 };
        case CLIENT_RESULT_TYPE_WARP:
            return (struct OnPacketResult) { .status = 0, .room = res.map };
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
        case 0:
        break;

        case 1: {
            READ_OR_ERROR(reader_u32, &npc);
            if (READER_AVAILABLE() == 4) {
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
            }
            struct ClientResult res = client_start_quest(client, qid, npc, false);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_KICK:
            case CLIENT_RESULT_TYPE_ERROR:
                return (struct OnPacketResult) { .status = -1 };
            case CLIENT_RESULT_TYPE_SUCCESS:
                return (struct OnPacketResult) { .status = 0, .room = -1 };
            case CLIENT_RESULT_TYPE_WARP:
                return (struct OnPacketResult) { .status = 0, .room = res.map };
            }
        }
        break;

        case 2: {
            READ_OR_ERROR(reader_u32, &npc);
            if (READER_AVAILABLE() == 4) {
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
            }
            uint8_t selection;
            struct ClientResult res = client_end_quest(client, qid, npc, false);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_KICK:
            case CLIENT_RESULT_TYPE_ERROR:
                return (struct OnPacketResult) { .status = -1 };
            case CLIENT_RESULT_TYPE_SUCCESS:
                return (struct OnPacketResult) { .status = 0, .room = -1 };
            case CLIENT_RESULT_TYPE_WARP:
                return (struct OnPacketResult) { .status = 0, .room = res.map };
            }
        }
        break;
        case 3:
        break;
        case 4: {
            READ_OR_ERROR(reader_u32, &npc);
            if (READER_AVAILABLE() == 4) {
                READ_OR_ERROR(reader_i16, &x);
                READ_OR_ERROR(reader_i16, &y);
            }
            struct ClientResult res = client_start_quest(client, qid, npc, true);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_KICK:
            case CLIENT_RESULT_TYPE_ERROR:
                return (struct OnPacketResult) { .status = -1 };
            case CLIENT_RESULT_TYPE_SUCCESS:
                return (struct OnPacketResult) { .status = 0, .room = -1 };
            case CLIENT_RESULT_TYPE_WARP:
                return (struct OnPacketResult) { .status = 0, .room = res.map };
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
            case CLIENT_RESULT_TYPE_KICK:
            case CLIENT_RESULT_TYPE_ERROR:
                return (struct OnPacketResult) { .status = -1 };
            case CLIENT_RESULT_TYPE_SUCCESS:
                return (struct OnPacketResult) { .status = 0, .room = -1 };
            case CLIENT_RESULT_TYPE_WARP:
                return (struct OnPacketResult) { .status = 0, .room = res.map };
            }
        }
        break;

        default:
            return (struct OnPacketResult) { .status = -1 };
        }
        READER_END();
    }
    break;

    case 0x00B7: {
    }
    break;

    case 0x00BC: {
        if (client->map.handle == NULL)
            return (struct OnPacketResult) { .status = 0, .room = -1 };

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

        if (map_move_monster(map, client->map.handle, activity, oid, x, y, fh, stance, len, packet + 25)) {
            uint8_t send[MOVE_MOB_RESPONSE_PACKET_LENGTH];
            move_monster_response_packet(oid, moveid, send);
            session_write(session, MOVE_MOB_RESPONSE_PACKET_LENGTH, send);
        }

        READER_END();

    }
    break;

    case 0x00C5: {
        if (client->map.handle == NULL)
            return (struct OnPacketResult) { .status = 0, .room = -1 };

        READER_BEGIN(size, packet);
        if (size == 6) {
            uint8_t data[6];
            uint8_t out[8];
            READ_OR_ERROR(reader_array, 6, data);
            npc_action_packet(6, data, out);
            session_write(session, 8, out);
        } else {
            uint8_t *data = malloc(size - 9); // TODO: what if 6 < size < 9?
            uint8_t *out = malloc(size - 7);
            READ_OR_ERROR(reader_array, size - 9, data);
            npc_action_packet(size - 9, data, out);
            free(data);
            session_write(session, size - 7, out);
            free(out);
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
            // TODO: Check if this client is allowed to pick up the drop
            switch (drop->type) {
            case DROP_TYPE_MESO: {
                client_gain_meso(client, drop->meso, true, false);
            }
            break;

            case DROP_TYPE_ITEM: {
                bool success;
                if (!client_gain_inventory_item(client, &drop->item, &success)) {
                    return (struct OnPacketResult) { .status = -1 };
                }

                if (success) {
                    {
                        uint8_t packet[ITEM_GAIN_PACKET_LENGTH];
                        item_gain_packet(drop->item.item.itemId, drop->item.quantity, packet);
                        session_write(session, ITEM_GAIN_PACKET_LENGTH, packet);
                    }

                    {
                        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                        size_t len = stat_change_packet(true, 0, NULL, packet);
                        session_write(session, len, packet);
                    }
                } else {
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
                }
            }
                break;

            case DROP_TYPE_EQUIP: {
                bool success;
                if (!client_gain_equipment(client, &drop->equip, false, &success)) {
                    return (struct OnPacketResult) { .status = -1 };
                }

                if (success) {
                    {
                        uint8_t packet[ITEM_GAIN_PACKET_LENGTH];
                        item_gain_packet(drop->equip.item.itemId, 1, packet);
                        session_write(session, ITEM_GAIN_PACKET_LENGTH, packet);
                    }

                    {
                        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                        size_t len = stat_change_packet(true, 0, NULL, packet);
                        session_write(session, len, packet);
                    }
                } else {
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
                }
            }
            }
        }
    }
    break;

    case 0x00CF: {
        READER_BEGIN(size, packet);
        READER_END();
        struct Map *map = room_get_context(session_get_room(session));
        session_enable_write(session);

        uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
        size_t len = add_player_to_map_packet(&client->character, packet);
        session_broadcast_to_room(session, len, packet);

        map_join(map, session, &client->map);
        session_foreach_in_room(session, notify_player_on_map, NULL);
        session_write(session, 2, (uint8_t[]) { 0x23, 0x00 }); // Force stat reset
        map_for_each_npc(map, notify_npc_on_map, session);
        map_for_each_drop(map, notify_drop_on_map, session);
        if (client->script != NULL) {
            struct ClientResult res = client_script_cont(client, 0);
            switch (res.type) {
            case CLIENT_RESULT_TYPE_KICK:
            case CLIENT_RESULT_TYPE_ERROR:
                return (struct OnPacketResult) { .status = -1 };
            case CLIENT_RESULT_TYPE_SUCCESS:
                return (struct OnPacketResult) { .status = 0, .room = -1 };
            case CLIENT_RESULT_TYPE_WARP:
                return (struct OnPacketResult) { .status = 0, .room = res.map };
            break;
            }
        }
    }
    break;

    default: {
        return (struct OnPacketResult) { .status = 0, .room = -1 };
    }
    break;
    }

    return (struct OnPacketResult) { .status = 0, .room = -1 };
}

static struct OnPacketResult on_unassigned_client_packet(struct Session *session, size_t size, uint8_t *packet)
{
    if (size < 2)
        return (struct OnPacketResult) { .status = -1 };

    struct Client *client = session_get_context(session);
    uint16_t opcode;
    memcpy(&opcode, packet, sizeof(uint16_t));

    if (opcode != 0x0014)
        return (struct OnPacketResult) { .status = -1 };

    uint32_t token;
    READER_BEGIN(size - 2, packet + 2);
    READ_OR_ERROR(reader_u32, &token);
    SKIP(2);
    READER_END();
    uint32_t id;
    if (!session_assign_token(session, token, &id))
        return (struct OnPacketResult) { .status = -1 };

    client->handlerType = PACKET_TYPE_LOGIN;
    client->handler = login_handler_create(client, id);
    if (client->handler == NULL)
        return (struct OnPacketResult) { .status = -1 };

    int fd = database_connection_lock(client->conn);
    if (fd == -2) {
        struct LoginHandlerResult res = login_handler_handle(client->handler, 0);
        if (res.status > 0) {
            session_set_event(session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet, false);
            return (struct OnPacketResult) { .status = res.status };
        } else {
            database_connection_unlock(client->conn);
            login_handler_destroy(client->handler);
            if (res.status < 0)
                return (struct OnPacketResult) { .status = -1 };
            session_write(session, res.size, res.packet);
            return (struct OnPacketResult) { .status = 0, .room = client->targetMap };
        }
    } else if (fd == -1) {
        return (struct OnPacketResult) { .status = -1 };
    } else {
            session_set_event(session, POLLIN, fd, on_database_lock_ready, false);
        return (struct OnPacketResult) { .status = POLLIN };
    }
}

static struct OnResumeResult on_resume_client_packet(struct Session *session, int fd, int status)
{
    struct Client *client = session_get_context(session);
    switch (client->handlerType) {
    case PACKET_TYPE_LOGIN: {
        struct LoginHandlerResult res = login_handler_handle(client->handler, status);
        if (res.status > 0) {
            if (res.status != session_get_event_disposition(session)) {
                session_set_event(session, res.status, -1, on_resume_client_packet, client->assigned);
            }
            return (struct OnResumeResult) { .status = res.status };
        } else if (res.status < 0) {
            database_connection_unlock(client->conn);
            login_handler_destroy(client->handler);
            return (struct OnResumeResult) { .status = res.status };
        }
        database_connection_unlock(client->conn);
        login_handler_destroy(client->handler);
        return (struct OnResumeResult) { .status = 0, .room = client->targetMap };
    }
    break;

    default:
        assert(0);
    }

    return (struct OnResumeResult) { .status = 0, .room = -1 };
}

static void on_client_disconnect(struct Session *session)
{
    struct Client *client = session_get_context(session);
    if (client->assigned) {
        struct Map *map = room_get_context(session_get_room(session));
        script_manager_free(client->script);

        uint8_t packet[REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH];
        remove_player_from_map_packet(client->character.id, packet);
        session_broadcast_to_room(session, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);

        map_leave(map, client->map.handle);

        client->handlerType = PACKET_TYPE_LOGOUT;
        client->handler = logout_handler_create(client);
        if (client->handler == NULL) {
            hash_set_u16_destroy(client->character.quests);
            hash_set_u16_destroy(client->character.completedQuests);
            free(client);
            return;
        }

        int fd = database_connection_lock(client->conn);
        if (fd == -2) {
            int status = logout_handler_handle(client->handler, 0);
            if (status > 0) {
                session_set_event(session, status, database_connection_get_fd(client->conn), on_resume_client_disconnect, true);
            } else {
                database_connection_unlock(client->conn);
                logout_handler_destroy(client->handler);
                hash_set_u16_destroy(client->character.quests);
                hash_set_u16_destroy(client->character.completedQuests);
                free(client);
            }
        } else if (fd == -1) {
            logout_handler_destroy(client->handler);
            hash_set_u16_destroy(client->character.quests);
            hash_set_u16_destroy(client->character.completedQuests);
            free(client);
        } else {
            session_set_event(session, POLLIN, fd, on_database_lock_ready, true);
        }
    }
}

static struct OnResumeResult on_resume_client_disconnect(struct Session *session, int fd, int status)
{
    struct Client *client = session_get_context(session);
    status = logout_handler_handle(client->handler, status);
    if (status > 0)
        return (struct OnResumeResult) { .status = status };

    database_connection_unlock(client->conn);
    logout_handler_destroy(client->handler);
    hash_set_u16_destroy(client->character.quests);
    hash_set_u16_destroy(client->character.completedQuests);
    free(client);
    return (struct OnResumeResult) { .status = 0 };
}

static bool on_client_join(struct Session *session, void *thread_ctx)
{
    struct Client *client = session_get_context(session);

    client->character.map = client->targetMap;

    if (!client->assigned) {
        client->assigned = true;

        {
            uint8_t packet[ENTER_MAP_PACKET_MAX_LENGTH];
            size_t len = enter_map_packet(&client->character, packet);
            session_write(session, len, packet);
        }

        session_write(session, 2, (uint8_t[]) { 0x23, 0x00 }); // Force stat reset

        {
            uint8_t packet[SET_GENDER_PACKET_LENGTH];
            set_gender_packet(client->character.gender, packet);
            session_write(session, SET_GENDER_PACKET_LENGTH, packet);
        }

        session_write(session, 3, (uint8_t[]) { 0x2F, 0x00, 0x01 });
    } else {
        uint8_t packet[CHANGE_MAP_PACKET_LENGTH];
        change_map_packet(&client->character, client->targetMap, client->targetPortal, packet);
        session_write(session, CHANGE_MAP_PACKET_LENGTH, packet);
    }

    client->conn = thread_ctx;
    //session_write(session, 3, (uint8_t[]) { 0xDE, 0x00, 0x00 }); // Disable UI
    //session_write(session, 3, (uint8_t[]) { 0xDD, 0x00, 0x00 }); // Lock UI
    //session_write(session, 2, (uint8_t[]) { 0x23, 0x00 }); // Force stat reset
    const struct PortalInfo *info = wz_get_portal_info(client->targetMap, client->targetPortal);
    client->character.x = info->x;
    client->character.y = info->y;
    client->character.stance = 6;
    client->character.fh = 0;
    {
        uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
        size_t len = add_player_to_map_packet(&client->character, packet);
        session_broadcast_to_room(session, len, packet);
    }
    return true;
}

static int on_room_create(struct Room *room, void *thread_ctx)
{
    struct Map *map = map_create(room);
    if (map == NULL)
        return -1;

    room_set_context(room, map);

    return 0;
}

static void on_room_destroy(struct Room *room)
{
    map_destroy(room_get_context(room));
}

static struct OnResumeResult on_database_lock_ready(struct Session *session, int fd, int status)
{
    struct Client *client = session_get_context(session);
    close(fd);
    switch (client->handlerType) {
    case PACKET_TYPE_LOGIN: {
        struct LoginHandlerResult res = login_handler_handle(client->handler, 0);
        if (res.status > 0) {
            session_set_event(session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet, client->assigned);
            return (struct OnResumeResult) { .status = res.status };
        }

        login_handler_destroy(client->handler);
        database_connection_unlock(client->conn);
        return (struct OnResumeResult) { .status = res.status, .room = client->targetMap };
    }
    break;

    case PACKET_TYPE_LOGOUT: {
        int status = logout_handler_handle(client->handler, 0);
        if (status > 0) {
            session_set_event(session, status, database_connection_get_fd(client->conn), on_resume_client_packet, client->assigned);
            return (struct OnResumeResult) { .status = status };
        }

        logout_handler_destroy(client->handler);
        database_connection_unlock(client->conn);
        return (struct OnResumeResult) { .status = status };
    }
    break;
    }
}

static void on_sigint(int sig)
{
    channel_server_stop(SERVER);
    signal(sig, SIG_DFL);
}

static void notify_player_on_map(struct Session *src, struct Session *dst, void *ctx)
{
    struct Client *c = session_get_context(dst);
    uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
    size_t len = add_player_to_map_packet(&c->character, packet);
    session_write(src, len, packet);
}

static void notify_npc_on_map(struct Npc *npc, void *ctx)
{
    {
        uint8_t packet[SPAWN_NPC_PACKET_LENGTH];
        spawn_npc_packet(npc->oid, npc->id, npc->x, npc->cy, npc->f == 1, npc->fh, npc->rx0, npc->rx1, packet);
        session_write(ctx, SPAWN_NPC_PACKET_LENGTH, packet);
    }
    {
        uint8_t packet[SPAWN_NPC_CONTROLLER_PACKET_LENGTH];
        spawn_npc_controller_packet(npc->oid, npc->id, npc->x, npc->cy, npc->f == 1, npc->fh, npc->rx0, npc->rx1, packet);
        session_write(ctx, SPAWN_NPC_CONTROLLER_PACKET_LENGTH, packet);
    }
}

static void notify_drop_on_map(struct Drop *drop, void *ctx)
{
    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[SPAWN_MESO_DROP_PACKET_LENGTH];
        spawn_meso_drop_packet(drop->oid, 0, drop->pos.x, drop->pos.y, 0, packet);
        session_write(ctx, SPAWN_MESO_DROP_PACKET_LENGTH, packet);
    }
    break;
    case DROP_TYPE_ITEM: {
        uint8_t packet[SPAWN_ITEM_DROP_PACKET_LENGTH];
        spawn_item_drop_packet(drop->oid, drop->item.item.itemId, 0, drop->pos.x, drop->pos.y, 0, packet);
        session_write(ctx, SPAWN_ITEM_DROP_PACKET_LENGTH, packet);
    }
    break;
    case DROP_TYPE_EQUIP: {
        uint8_t packet[SPAWN_ITEM_DROP_PACKET_LENGTH];
        spawn_item_drop_packet(drop->oid, drop->equip.item.itemId, 0, drop->pos.x, drop->pos.y, 0, packet);
        session_write(ctx, SPAWN_ITEM_DROP_PACKET_LENGTH, packet);
    }
    break;
    }
}


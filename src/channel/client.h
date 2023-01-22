#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>

#include <lua.h>

#include "server.h"
#include "../packet.h"
#include "map.h"
#include "../character.h"
#include "../database.h"

enum PacketType {
    PACKET_TYPE_LOGIN,
    PACKET_TYPE_LOGOUT,
};

enum ScriptState {
    SCRIPT_STATE_OK,
    SCRIPT_STATE_YES_NO,
    SCRIPT_STATE_SIMPLE,
    SCRIPT_STATE_NEXT,
    SCRIPT_STATE_PREV_NEXT,
    SCRIPT_STATE_PREV,
    SCRIPT_STATE_ACCEPT_DECILNE,
    SCRIPT_STATE_WARP,
};

struct Client {
    struct Session *session;
    struct MapHandleContainer map;
    //struct Map *map;
    struct DatabaseConnection *conn;
    struct {
        struct ScriptManager *quest;
        struct ScriptManager *npc;
        struct ScriptManager *portal;
    } managers;
    struct LockQueue *lockQueue;
    enum PacketType handlerType;
    // TODO: Make this a handler queue
    void *handler;
    struct Character character;
    struct ScriptHandle *script;
    enum ScriptState scriptState;
    uint16_t qid;
    uint32_t npc;
};

enum ClientResultType {
    CLIENT_RESULT_TYPE_KICK = -2,
    CLIENT_RESULT_TYPE_ERROR,
    CLIENT_RESULT_TYPE_SUCCESS,
    CLIENT_RESULT_TYPE_WARP,
};

struct ClientResult {
    enum ClientResultType type;
    union {
        const char *reason;
        struct {
            uint32_t map;
            uint8_t portal;
        };
    };
};

void client_set_hp(struct Client *client, int16_t hp);
void client_adjust_hp(struct Client *client, int32_t hp);
void client_set_mp(struct Client *client, int16_t mp);
void client_adjust_mp(struct Client *client, int16_t mp);
void client_adjust_sp(struct Client *client, int16_t sp);
void client_raise_str(struct Client *client);
void client_raise_dex(struct Client *client);
void client_raise_int(struct Client *client);
void client_raise_luk(struct Client *client);
bool client_assign_sp(struct Client *client, uint32_t id);
void client_change_job(struct Client *client, enum Job job);
void client_gain_exp(struct Client *client, int32_t exp, bool reward);
void client_gain_meso(struct Client *client, int32_t mesos, bool pickup, bool reward);
void client_adjust_fame(struct Client *client, int16_t fame);
bool client_has_item(struct Client *client, uint32_t id);
bool client_gain_items(struct Client *client, size_t len, const uint32_t *ids, const int16_t *counts, bool reward, bool *success);
bool client_gain_inventory_item(struct Client *client, const struct InventoryItem *item, bool *success);
bool client_gain_equipment(struct Client *client, const struct Equipment *item, bool equip, bool *success);
bool client_remove_item(struct Client *client, uint8_t inventory, uint8_t src, int16_t amount, bool *success, struct InventoryItem *item);
bool client_remove_equip(struct Client *client, bool equipped, uint8_t src, bool *success, struct Equipment *equip);
bool client_move_item(struct Client *client, uint8_t inventory, uint8_t src, uint8_t dst);
bool client_equip(struct Client *client, uint8_t src, enum EquipSlot dst);
bool client_unequip(struct Client *client, enum EquipSlot src, uint8_t dst);
bool client_use_item(struct Client *client, uint8_t slot, uint32_t id);
bool client_use_item_immediate(struct Client *client, uint32_t id);
bool client_is_quest_started(struct Client *client, uint16_t qid);
struct ClientResult client_npc_talk(struct Client *client, uint32_t npc);
struct ClientResult client_start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted);
bool client_start_quest_now(struct Client *client, bool *success);
struct ClientResult client_end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted);
bool client_end_quest_now(struct Client *client, bool *success);
bool client_forfeit_quest(struct Client *client, uint16_t qid);
struct ClientResult client_script_cont(struct Client *client, uint32_t action);
void client_kill_monster(struct Client *client, uint32_t id);
void client_send_ok(struct Client *client, size_t msg_len, const char *msg);
void client_send_yes_no(struct Client *client, size_t msg_len, const char *msg);
void client_send_simple(struct Client *client, size_t msg_len, const char *msg);
void client_send_next(struct Client *client, size_t msg_len, const char *msg);
void client_send_prev_next(struct Client *client, size_t msg_len, const char *msg);
void client_send_prev(struct Client *client, size_t msg_len, const char *msg);
void client_send_accept_decline(struct Client *client, size_t msg_len, const char *msg);
void client_warp(struct Client *client, uint32_t map, uint8_t portal);
void client_reset_stats(struct Client *client);
struct ClientResult client_portal_script(struct Client *client, const char *portal);
void client_enable_actions(struct Client *client);

#endif


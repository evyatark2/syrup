#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>

#include <lua.h>

#include "server.h"
#include "drop.h"
//#include "../character.h"
#include "../database.h"
#include "scripting/script-manager.h"

struct Client;

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

enum ClientResultType {
    CLIENT_RESULT_TYPE_BAN = -2, // The client packet-edited
    CLIENT_RESULT_TYPE_ERROR = -1,
    CLIENT_RESULT_TYPE_SUCCESS = 0,
    CLIENT_RESULT_TYPE_WARP = 1,
};

struct ClientResult {
    enum ClientResultType type;
    union {
        const char *reason; // Used for kicking
        struct {
            uint32_t map;
            uint8_t portal;
        };
    };
};

struct ClientContResult {
    int status;
    int fd;
    uint32_t map;
};

struct Client *client_create(struct Session *session, struct DatabaseConnection *conn, struct ScriptManager *quest_manager, struct ScriptManager *portal_mananger, struct ScriptManager *npc_manager);
void client_destroy(struct Client *client);
struct Session *client_get_session(struct Client *client);
void client_login_start(struct Client *client, uint32_t id);
void client_logout_start(struct Client *client);
struct ClientContResult client_cont(struct Client *client, int status);
void client_update_conn(struct Client *client, struct DatabaseConnection *conn);
const struct Character *client_get_character(struct Client *client);
uint32_t client_get_active_npc(struct Client *client);
struct MapHandleContainer *client_get_map(struct Client *client);
bool client_announce_monster(struct Client *client);
bool client_announce_drop(struct Client *client, uint32_t owner_id, uint32_t dropper_oid, bool player_drop, const struct Drop *drop);
bool client_announce_spawn_drop(struct Client *client, uint32_t owner_id, uint32_t dropper_oid, bool player_drop, const struct Drop *drop);
void client_update_player_pos(struct Client *client, int16_t x, int16_t y, uint16_t fh, uint8_t stance);
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
struct ClientResult client_open_shop(struct Client *client, uint32_t id);
struct ClientResult client_buy(struct Client *client, uint16_t pos, uint32_t id, int16_t quantity, int32_t price);
struct ClientResult client_sell(struct Client *client, uint16_t pos, uint32_t id, int16_t quantity);
bool client_close_shop(struct Client *client);
bool client_is_in_shop(struct Client *client);
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


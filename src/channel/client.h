#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>

#include <lua.h>

#include "../database.h"
#include "drop.h"
#include "life.h"
#include "scripting/script-manager.h"
#include "server.h"

struct Client;

enum PacketType {
    PACKET_TYPE_LOGIN,
    PACKET_TYPE_LOGOUT,
};

enum ScriptState {
    SCRIPT_STATE_OK,
    SCRIPT_STATE_YES_NO,
    SCRIPT_STATE_GET_NUMBER,
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
    CLIENT_RESULT_TYPE_NEXT = 1,
};

struct ClientResult {
    enum ClientResultType type;
    union {
        const char *reason; // Used for kicking
    };
};

struct ClientContResult {
    int status;
    int fd;
    uint32_t map;
};

struct Client *client_create(struct Session *session, struct DatabaseConnection *conn, struct ScriptManager *quest_manager, struct ScriptManager *portal_mananger, struct ScriptManager *npc_manager, struct ScriptManager *map_manager);
void client_destroy(struct Client *client);
struct Session *client_get_session(struct Client *client);
void client_login_start(struct Client *client, uint32_t id);
void client_logout_start(struct Client *client);
struct ClientContResult client_cont(struct Client *client, int status);
void client_update_conn(struct Client *client, struct DatabaseConnection *conn);
const struct Character *client_get_character(struct Client *client);
uint32_t client_get_active_npc(struct Client *client);
struct MapHandleContainer *client_get_map(struct Client *client);
void client_announce_self_to_map(struct Client *client);
void client_announce_add_player(struct Client *client, const struct Character *chr);
void client_announce_add_npc(struct Client *client, const struct Npc *npc);
void client_announce_monster(struct Client *client, const struct Monster *monster);
bool client_announce_drop(struct Client *client, uint32_t owner_id, uint32_t dropper_oid, uint8_t type, bool player_drop, const struct Drop *drop);
bool client_announce_spawn_drop(struct Client *client, uint32_t owner_id, uint32_t dropper_oid, uint8_t type, bool player_drop, const struct Drop *drop);
void client_update_player_pos(struct Client *client, int16_t x, int16_t y, uint16_t fh, uint8_t stance);
void client_set_hp(struct Client *client, int16_t hp);
void client_set_hp_now(struct Client *client, int16_t hp);
void client_adjust_hp(struct Client *client, int32_t hp);
void client_adjust_hp_now(struct Client *client, int32_t hp);
void client_set_mp(struct Client *client, int16_t mp);
void client_adjust_mp(struct Client *client, int16_t mp);
void client_adjust_mp_now(struct Client *client, int32_t mp);
void client_adjust_sp(struct Client *client, int16_t sp);
void client_adjust_str(struct Client *client, int16_t str);
void client_adjust_dex(struct Client *client, int16_t dex);
void client_adjust_int(struct Client *client, int16_t int_);
void client_adjust_luk(struct Client *client, int16_t luk);
bool client_assign_sp(struct Client *client, uint32_t id);
void client_change_job(struct Client *client, enum Job job);
void client_gain_exp(struct Client *client, int32_t exp, bool reward);
void client_gain_meso(struct Client *client, int32_t mesos, bool pickup, bool reward);
void client_adjust_fame(struct Client *client, int16_t fame);
void client_commit_stats(struct Client *client);
bool client_has_item(struct Client *client, uint32_t id, int16_t qty);
bool client_gain_items(struct Client *client, size_t len, const uint32_t *ids, const int16_t *counts, bool reward, bool *success);

enum InventoryGainResult {
    INVENTORY_GAIN_RESULT_SUCCESS,
    INVENTORY_GAIN_RESULT_FULL,
    INVENTORY_GAIN_RESULT_UNAVAILABLE
};

bool client_gain_inventory_item(struct Client *client, const struct InventoryItem *item, enum InventoryGainResult *success);
bool client_gain_equipment(struct Client *client, const struct Equipment *item, bool equip, enum InventoryGainResult *success);
bool client_remove_item(struct Client *client, uint8_t inventory, uint8_t src, int16_t amount, bool *success, struct InventoryItem *item);
bool client_use_projectile(struct Client *client, int16_t amount, bool *success);
bool client_remove_equip(struct Client *client, bool equipped, uint8_t src, bool *success, struct Equipment *equip);
bool client_move_item(struct Client *client, uint8_t inventory, uint8_t src, uint8_t dst);
bool client_equip(struct Client *client, uint8_t src, enum EquipSlot dst);
bool client_unequip(struct Client *client, enum EquipSlot src, uint8_t dst);
bool client_use_item(struct Client *client, uint8_t slot, uint32_t id);
bool client_use_item_immediate(struct Client *client, uint32_t id);
bool client_is_quest_started(struct Client *client, uint16_t qid);
bool client_is_quest_complete(struct Client *client, uint16_t qid);
struct ClientResult client_npc_talk(struct Client *client, uint32_t npc);
struct ClientResult client_launch_map_script(struct Client *client, const char *script_name);
struct ClientResult client_start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted);
struct ClientResult client_regain_quest_item(struct Client *client, uint16_t qid, uint32_t id);
const char *client_get_quest_info(struct Client *client, uint16_t info);
int client_set_quest_info(struct Client *client, uint16_t info, const char *value);
bool client_start_quest_now(struct Client *client, bool *success);
struct ClientResult client_end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted);
bool client_end_quest_now(struct Client *client, bool *success);
bool client_forfeit_quest(struct Client *client, uint16_t qid);
struct ClientResult client_script_cont(struct Client *client, uint8_t prev, uint8_t action, uint32_t selection);
void client_close_script(struct Client *client);
void client_kill_monster(struct Client *client, uint32_t id);
void client_destroy_reactor(struct Client *client);
struct ClientResult client_open_shop(struct Client *client, uint32_t id);
struct ClientResult client_buy(struct Client *client, uint16_t pos, uint32_t id, int16_t quantity, int32_t price);
struct ClientResult client_sell(struct Client *client, uint16_t pos, uint32_t id, int16_t quantity);
struct ClientResult client_recharge(struct Client *client, uint16_t pos);
bool client_close_shop(struct Client *client);
bool client_is_in_shop(struct Client *client);
void client_send_ok(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker);
void client_send_yes_no(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker);
void client_send_simple(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker, uint32_t selection_count);
void client_send_next(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker);
void client_send_prev_next(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker);
void client_send_prev(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker);
void client_send_accept_decline(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker);
void client_send_get_number(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker, int32_t def, int32_t min, int32_t max);
void client_message(struct Client *client, const char *msg);
void client_warp(struct Client *client, uint32_t map, uint8_t portal);
void client_reset_stats(struct Client *client);
struct ClientResult client_launch_portal_script(struct Client *client, const char *portal);
void client_enable_actions(struct Client *client);
void client_toggle_auto_pickup(struct Client *client);
bool client_is_auto_pickup_enabled(struct Client *client);
bool client_apply_skill(struct Client *client, uint32_t skill_id, uint8_t *level);
bool client_add_key(struct Client *client, uint32_t key, uint8_t type, uint32_t action);
bool client_add_skill_key(struct Client *client, uint32_t key, uint32_t skill_id);
bool client_remove_key(struct Client *client, uint32_t key, uint32_t action);
bool client_sit(struct Client *client, uint32_t id);
bool client_sit_on_map_seat(struct Client *client, uint16_t id);
bool client_stand_up(struct Client *client);
bool client_open_storage(struct Client *client);
void client_show_info(struct Client *client, const char *path);
void client_show_intro(struct Client *client, const char *path);

#endif


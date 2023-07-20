#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>

#include <lua.h>

#include "../database.h"
#include "drop.h"
#include "life.h"
#include "session.h"
#include "stat.h"

struct Client;

enum PacketType {
    PACKET_TYPE_NONE,
    PACKET_TYPE_LOGIN,
    PACKET_TYPE_LOGOUT,
};

enum ClientDialogueState {
    CLIENT_DIALOGUE_STATE_OK,
    CLIENT_DIALOGUE_STATE_YES_NO,
    CLIENT_DIALOGUE_STATE_GET_NUMBER,
    CLIENT_DIALOGUE_STATE_SIMPLE,
    CLIENT_DIALOGUE_STATE_NEXT,
    CLIENT_DIALOGUE_STATE_PREV_NEXT,
    CLIENT_DIALOGUE_STATE_PREV,
    CLIENT_DIALOGUE_STATE_ACCEPT_DECILNE,
    CLIENT_DIALOGUE_STATE_WARP,
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
};

struct ClientCommand;

struct ClientSave;

int clients_init(void);
void clients_terminate(void);

struct Client *client_create(struct Character *chr);
void client_destroy(struct Client *client);
struct Character *client_get_character(struct Client *client);
struct Session *client_get_session(struct Client *client);
void client_login_start(struct Client *client, uint32_t id);
int client_save_start(struct Client *client);
void client_logout_start(struct Client *client);
void client_set_map(struct Client *client, uint32_t map);
uint32_t client_map(struct Client *client);
uint8_t client_skin(struct Client *client);
uint32_t client_face(struct Client *client);
uint32_t client_hair(struct Client *client);
uint8_t client_level(struct Client *client);
uint16_t client_job(struct Client *client);
// External sources (i.e. monster damage) can kill the player while internal sources (skill requirement) can't
// Returns if the adjustment was successful (always true from external sources)
void client_set_hp(struct Client *client, int16_t hp);
bool client_adjust_hp(struct Client *client, bool external, int16_t hp);
void client_adjust_hp_precent(struct Client *client, uint8_t hpR);
int16_t client_hp(struct Client *client);
void client_set_max_hp(struct Client *client, int16_t hp);
int16_t client_max_hp(struct Client *client);
void client_set_mp(struct Client *client, int16_t mp);
bool client_adjust_mp(struct Client *client, int16_t mp);
void client_adjust_mp_precent(struct Client *client, uint8_t mpR);
int16_t client_mp(struct Client *client);
void client_set_max_mp(struct Client *client, int16_t hp);
int16_t client_max_mp(struct Client *client);
bool client_adjust_hp_mp(struct Client *client, int16_t hp, int16_t mp);
void client_adjust_sp(struct Client *client, int16_t sp);
int16_t client_sp(struct Client *client);
bool client_adjust_str(struct Client *client, int16_t str);
int16_t client_str(struct Client *client);
bool client_adjust_dex(struct Client *client, int16_t dex);
int16_t client_dex(struct Client *client);
bool client_adjust_int(struct Client *client, int16_t int_);
int16_t client_int(struct Client *client);
bool client_adjust_luk(struct Client *client, int16_t luk);
int16_t client_luk(struct Client *client);
bool client_adjust_ap(struct Client *client, int16_t ap);
int16_t client_ap(struct Client *client);
bool client_assign_sp(struct Client *client, uint32_t id, int8_t *level, int8_t *master);
void client_change_job(struct Client *client, enum Job job);
uint8_t client_gain_exp(struct Client *client, int32_t exp, uint32_t *stats);
int32_t client_exp(struct Client *client);
/**
 * Adjusts the client's meso count.
 *
 * \p client The client
 * \p underflow Whether exact meso count is required
 * \p mesos The amount
 *
 * \returns If underflow is false, true if the meso deduction was successful; otherwise, true
 */
bool client_adjust_mesos(struct Client *client, bool underflow, int32_t mesos);
int32_t client_meso(struct Client *client);
void client_adjust_fame(struct Client *client, int16_t fame);
int16_t client_fame(struct Client *client);
uint32_t client_commit_stats(struct Client *client);
bool client_has_item(struct Client *client, uint32_t id, int16_t qty);
uint8_t client_inventory_slot_count(struct Client *client, uint8_t inv);
uint8_t client_equip_slot_count(struct Client *client);

enum InventoryGainResult {
    INVENTORY_GAIN_RESULT_SUCCESS,
    INVENTORY_GAIN_RESULT_FULL,
    INVENTORY_GAIN_RESULT_UNAVAILABLE
};

bool client_gain_items(struct Client *client, size_t len, const uint32_t *ids, const int16_t *counts, size_t *count, struct InventoryModify **changes);
bool client_gain_inventory_item(struct Client *client, const struct InventoryItem *item, size_t *count, struct InventoryModify **changes);
void client_decrease_quest_item(struct Client *client, uint32_t id, int16_t quantity);
bool client_gain_equipment(struct Client *client, const struct Equipment *item, struct InventoryModify *change);
bool client_remove_item(struct Client *client, uint8_t inventory, uint8_t src, int16_t amount, struct InventoryModify *change, struct InventoryItem *item);
uint32_t client_get_item(struct Client *client, uint8_t inventory, uint8_t slot);
int16_t client_remaining_quest_item_quantity(struct Client *client, uint32_t id);

bool client_use_projectile(struct Client *client, int16_t amount, uint32_t *id, struct InventoryModify *change);
bool client_remove_equip(struct Client *client, bool equipped, uint8_t src, struct InventoryModify *change, struct Equipment *equip);
uint8_t client_move_item(struct Client *client, uint8_t inventory, uint8_t src, uint8_t dst, struct InventoryModify *changes);
bool client_equip(struct Client *client, uint8_t src, enum EquipSlot dst, struct InventoryModify *change);
bool client_unequip(struct Client *client, enum EquipSlot src, uint8_t dst, struct InventoryModify *change);
uint32_t client_get_equip(struct Client *client, bool equipped, uint8_t slot);
bool client_has_use_item(struct Client *client, uint8_t slot, uint32_t id);
bool client_record_monster_book_entry(struct Client *client, uint32_t id, uint8_t *count);
bool client_check_start_quest_requirements(struct Client *client, const struct QuestInfo *info, uint32_t npc);
bool client_check_end_quest_requirements(struct Client *client, const struct QuestInfo *info, uint32_t npc);
void client_set_npc(struct Client *client, uint32_t id);
uint32_t client_get_npc(struct Client *client);
void client_set_quest(struct Client *client, uint32_t id);
uint32_t client_get_quest(struct Client *client);
bool client_add_quest(struct Client *client, uint16_t qid, size_t count, uint32_t *ids, bool *success);
bool client_is_quest_started(struct Client *client, uint16_t qid);
bool client_remove_quest(struct Client *client, uint16_t qid);
bool client_complete_quest(struct Client *client, uint16_t qid, time_t time);
bool client_is_quest_complete(struct Client *client, uint16_t qid);
bool client_has_empty_slot_in_each_inventory(struct Client *client);
struct ClientResult client_regain_quest_item(struct Client *client, uint16_t qid, uint32_t id);
const char *client_get_quest_info(struct Client *client, uint16_t info);
bool client_set_quest_info(struct Client *client, uint16_t info, const char *value);
bool client_start_quest_now(struct Client *client, bool *success);
void client_close_script(struct Client *client);
void client_kill_monster(struct Client *client, uint32_t id, void (*f)(uint16_t qid, size_t progress_count, int32_t *progress, void *ctx), void *ctx_);
void client_destroy_reactor(struct Client *client);
bool client_open_shop(struct Client *client, uint32_t id);
bool client_recharge(struct Client *client, uint16_t pos);
bool client_close_shop(struct Client *client);
uint32_t client_shop(struct Client *client);
void client_set_dialogue_state(struct Client *client, enum ClientDialogueState state, ...);
bool client_is_dialogue_option_legal(struct Client *client, uint8_t prev);
bool client_dialogue_is_action_valid(struct Client *client, uint8_t action, uint32_t selection, uint32_t *script_action);
void client_message(struct Client *client, const char *msg);
bool client_warp(struct Client *client, uint32_t map, uint8_t portal);
void client_reset_stats(struct Client *client);
void client_enable_actions(struct Client *client);
void client_toggle_auto_pickup(struct Client *client);
bool client_is_auto_pickup_enabled(struct Client *client);
bool client_has_skill(struct Client *client, uint32_t skill_id, int8_t *level);
bool client_gain_skill(struct Client *client, uint32_t skill_id, int8_t level, int8_t master);
bool client_add_key(struct Client *client, uint32_t key, uint8_t type, uint32_t action);
bool client_add_skill_key(struct Client *client, uint32_t key, uint32_t skill_id);
bool client_remove_key(struct Client *client, uint32_t key, uint32_t action);
bool client_sit(struct Client *client, uint32_t id);
bool client_sit_on_map_seat(struct Client *client, uint16_t id);
bool client_stand_up(struct Client *client);
bool client_open_storage(struct Client *client);
void client_show_info(struct Client *client, const char *path);
void client_show_intro(struct Client *client, const char *path);
void client_create_party(struct Client *client);
void client_invite_to_party(struct Client *client, uint8_t name_len, const char *name);
void client_reject_party_invitaion(struct Client *client, uint8_t name_len, const char *name);
void client_announce_party_join(struct Client *client, uint32_t id);
void client_announce_party_leave(struct Client *client, uint32_t id);
void client_announce_party_kick(struct Client *client, uint32_t id);
void client_announce_party_disband(struct Client *client);
void client_announce_party_change_online_status(struct Client *client, uint32_t id);
void client_announce_party_change_leader(struct Client *client, uint32_t id);

#endif


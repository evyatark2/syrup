#ifndef USER_H
#define USER_H

#include <sys/types.h> // ssize_t

#include "../character.h"
#include "session.h"
#include "scripting/script-manager.h"

struct User;

struct User *user_create(struct Session *session, struct Character *chr, struct ScriptManager *quest_manager, struct ScriptManager *portal_mananger, struct ScriptManager *npc_manager, struct ScriptManager *map_manager);
void user_destroy(struct User *user);
int user_portal(struct User *user, uint32_t target, size_t len, char *portal, uint32_t *out_map, uint8_t *out_portal);
struct Character *user_get_character(struct User *user);
struct Session *user_get_session(struct User *user);
void user_login_start(struct User *user, uint32_t id);
int user_save_start(struct User *user);
void user_logout_start(struct User *user);
void user_new_map(struct User *user);
struct UserContResult user_resume(struct User *user, int status);
int user_change_map(struct User *user, uint32_t target, uint8_t targetPortal);
uint32_t user_get_active_npc(struct User *user);
uint16_t user_get_active_quest(struct User *user);
void user_set_hp(struct User *user, int16_t hp);
void user_adjust_hp(struct User *user, int32_t hp);
void user_set_mp(struct User *user, int16_t mp);
void user_adjust_mp(struct User *user, int16_t mp);
int16_t user_mp(struct User *user);
int32_t user_meso(struct User *user);
void user_adjust_sp(struct User *user, int16_t sp);
bool user_assign_stat(struct User *user, uint32_t stat);
void user_assign_sp(struct User *user, uint32_t id);
void user_change_job(struct User *user, enum Job job);
bool user_gain_exp(struct User *user, int32_t exp, bool reward, bool *leveled);
bool user_gain_meso(struct User *user, int32_t mesos, bool pickup, bool reward);
void user_adjust_fame(struct User *user, int16_t fame);
bool user_commit_stats(struct User *user, uint32_t stats);
bool user_has_item(struct User *user, uint32_t id, int16_t qty);
bool user_gain_items(struct User *user, size_t len, const uint32_t *ids, const int16_t *counts, bool reward, bool *success);
bool user_has_space_for(struct User *user, uint32_t id, int16_t quantity);

bool user_gain_inventory_item(struct User *user, const struct InventoryItem *item, uint8_t *effect);
bool user_gain_equipment(struct User *user, const struct Equipment *item);
bool user_remove_item(struct User *user, uint8_t inventory, uint8_t src, int16_t amount, struct InventoryItem *item);
bool user_use_projectile(struct User *user, int16_t amount, uint32_t *id);
bool user_remove_equip(struct User *user, bool equipped, uint8_t src, struct Equipment *equip);
bool user_move_item(struct User *user, uint8_t inventory, uint8_t src, uint8_t dst);
bool user_equip(struct User *user, uint8_t src, enum EquipSlot dst);
bool user_unequip(struct User *user, enum EquipSlot src, uint8_t dst);
bool user_use_item(struct User *user, uint8_t slot, uint32_t id);
bool user_use_item_immediate(struct User *user, uint32_t id);
bool user_is_quest_started(struct User *user, uint16_t qid);
bool user_is_quest_complete(struct User *user, uint16_t qid);
void user_talk_npc(struct User *user, uint32_t npc);
void user_portal_script(struct User *user, const char *portal);
ssize_t user_start_quest(struct User *user, uint16_t qid, uint32_t npc, bool scripted, uint32_t **item_ids);
void user_regain_quest_item(struct User *user, uint16_t qid, uint32_t id);
const char *user_get_quest_info(struct User *user, uint16_t info);
int user_set_quest_info(struct User *user, uint16_t info, const char *value);
bool user_start_quest_now(struct User *user, bool *success);
bool user_end_quest(struct User *user, uint16_t qid, uint32_t npc, bool scripted);
bool user_end_quest_now(struct User *user);
bool user_forfeit_quest(struct User *user, uint16_t qid);
bool user_script_cont(struct User *user, uint8_t prev, uint8_t action, uint32_t selection);
void user_close_script(struct User *user);
bool user_kill_monsters(struct User *user, size_t count, uint32_t *ids, bool *leveled);
bool user_take_damage(struct User *user, int32_t damage);
bool user_chair(struct User *user, uint16_t id);
void user_destroy_reactor(struct User *user);
void user_open_shop(struct User *user, uint32_t id);
void user_buy(struct User *user, uint16_t pos, uint32_t id, int16_t quantity, int32_t price);
void user_sell(struct User *user, uint16_t pos, uint32_t id, int16_t quantity);
void user_recharge(struct User *user, uint16_t pos);
bool user_close_shop(struct User *user);
bool user_is_in_shop(struct User *user);
void user_send_ok(struct User *user, size_t msg_len, const char *msg, uint8_t speaker);
void user_send_yes_no(struct User *user, size_t msg_len, const char *msg, uint8_t speaker);
void user_send_simple(struct User *user, size_t msg_len, const char *msg, uint8_t speaker, uint32_t selection_count);
void user_send_next(struct User *user, size_t msg_len, const char *msg, uint8_t speaker);
void user_send_prev_next(struct User *user, size_t msg_len, const char *msg, uint8_t speaker);
void user_send_prev(struct User *user, size_t msg_len, const char *msg, uint8_t speaker);
void user_send_accept_decline(struct User *user, size_t msg_len, const char *msg, uint8_t speaker);
void user_send_get_number(struct User *user, size_t msg_len, const char *msg, uint8_t speaker, int32_t def, int32_t min, int32_t max);
void user_message(struct User *user, const char *msg);
bool user_warp(struct User *user, uint32_t map, uint8_t portal);
void user_reset_stats(struct User *user);
void user_launch_portal_script(struct User *user, const char *portal);
void user_launch_map_script(struct User *user, uint32_t map);
void user_enable_actions(struct User *user);
void user_toggle_auto_pickup(struct User *user);
bool user_is_auto_pickup_enabled(struct User *user);
bool user_use_skill(struct User *user, uint32_t skill, int8_t *level, uint32_t *projectile);
bool user_has_skill(struct User *user, uint32_t skill_id, uint8_t *level);
bool user_add_key(struct User *user, uint32_t key, uint8_t type, uint32_t action);
bool user_add_skill_key(struct User *user, uint32_t key, uint32_t skill_id);
bool user_remove_key(struct User *user, uint32_t key, uint32_t action);
bool user_sit(struct User *user, uint32_t id);
bool user_sit_on_map_seat(struct User *user, uint16_t id);
bool user_stand_up(struct User *user);
bool user_open_storage(struct User *user);
void user_show_info(struct User *user, const char *path);
void user_show_intro(struct User *user, const char *path);
void user_create_party(struct User *user);
void user_invite_to_party(struct User *user, uint8_t name_len, const char *name);
void user_reject_party_invitaion(struct User *user, uint8_t name_len, const char *name);
void user_announce_party_join(struct User *user, uint32_t id);
void user_announce_party_leave(struct User *user, uint32_t id);
void user_announce_party_kick(struct User *user, uint32_t id);
void user_announce_party_disband(struct User *user);
void user_announce_party_change_online_status(struct User *user, uint32_t id);
void user_announce_party_change_leader(struct User *user, uint32_t id);

#endif


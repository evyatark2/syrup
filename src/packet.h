#ifndef PACKET_H
#define PACKET_H

#include <stddef.h>
#include <stdint.h>

#include <sys/time.h>

#include "character.h"

enum PicStatus {
    PIC_STATUS_REGISTER,
    PIC_STATUS_REQUEST,
    PIC_STATUS_DISABLED,
};

#define LOGIN_SUCCESS_PACKET_MAX_LENGTH 54
size_t login_success_packet(uint32_t id, uint8_t gender, uint8_t name_len, char *name, enum PicStatus pic, uint8_t *packet);

enum LoginFailureReason {
    LOGIN_FAILURE_REASON_INCORRECT_PASSWORD = 4,
    LOGIN_FAILURE_REASON_INCORRECT_USERNAME = 5,
    LOGIN_FAILURE_REASON_SYSTEM_ERROR_6 = 6,
    LOGIN_FAILURE_REASON_LOGGED_IN = 7,
    LOGIN_FAILURE_REASON_SYSTEM_ERROR_8 = 8,
    LOGIN_FAILURE_REASON_SYSTEM_ERROR_9 = 9,
    LOGIN_FAILURE_REASON_ADULTS_ONLY = 11,
    LOGIN_FAILURE_REASON_NOT_A_GM_IP = 13,
    LOGIN_FAILURE_REASON_WRONG_GATEWAY_14 = 14,
    LOGIN_FAILURE_REASON_UNVERIFIED_ACCOUNT_16 = 16,
    LOGIN_FAILURE_REASON_WRONG_GATEWAY_17 = 17,
    LOGIN_FAILURE_REASON_UNVERIFIED_ACCOUNT_21 = 21,
    LOGIN_FAILURE_REASON_TOS = 23
};

#define LOGIN_FAILURE_PACKET_LENGTH 8
void login_failure_packet(enum LoginFailureReason reason, uint8_t *packet);

enum PinPacketMode {
    PIN_PACKET_MODE_ACCEPT,
    PIN_PACKET_MODE_REGISTER,
    PIN_PACKET_MODE_DENY,
    PIN_PACKET_MODE_SYSTEM_ERROR,
    PIN_PACKET_MODE_REQUEST,
};

#define PIN_PACKET_LENGTH 3
void pin_packet(enum PinPacketMode mode, uint8_t *packet);

enum WorldFlag {
    WORLD_FLAG_NONE,
    WORLD_FLAG_EVENT,
    WORLD_FLAG_NEW,
    WORLD_FLAG_HOT
};

#define SERVER_LIST_PACKET_MAX_LENGTH 65535
size_t server_list_packet(uint8_t world, enum WorldFlag flag, uint16_t mes_len, char *mes, uint8_t *packet);

#define SERVER_LIST_END_PACKET_LENGTH 3
void server_list_end_packet(uint8_t *packet);

enum ServerStatus {
    SERVER_STATUS_NORMAL,
    SERVER_STATUS_CRITICAL,
    SERVER_STATUS_FULL
};

#define SERVER_STATUS_PACKET_LENGTH 4
void server_status_packet(enum ServerStatus status, uint8_t *packet);

#define CHARACTER_LIST_PACKET_MAX_LENGTH 2673 // Assuming a 6 character per world limit
size_t character_list_packet(enum LoginFailureReason status, uint8_t char_count, struct CharacterStats *chars, uint8_t pic, uint8_t *packet);

#define CHANNEL_IP_PACKET_LENGTH 19
void channel_ip_packet(uint32_t addr, uint16_t port, uint32_t token, uint8_t *packet);

#define LOGIN_ERROR_PACKET_LENGTH 4
void login_error_packet(uint8_t err, uint8_t *packet);

#define NAME_CHECK_RESPONSE_PACKET_MAX_LENGTH 17
size_t name_check_response_packet(uint8_t name_len, char *name, bool available, uint8_t *packet);

#define CREATE_CHARACTER_RESPONSE_PACKET_LENGTH 162
size_t create_character_response_packet(struct CharacterStats *chr, uint8_t *packet);

#define ENTER_MAP_PACKET_MAX_LENGTH 65535
size_t enter_map_packet(struct Character *chr, uint8_t *packet);

#define SET_GENDER_PACKET_LENGTH 3
void set_gender_packet(bool gender, uint8_t *packet);

#define CHANGE_MAP_PACKET_LENGTH 27
void change_map_packet(struct Character *chr, uint32_t to, uint8_t portal, uint8_t *packet);

#define ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH 554
size_t add_player_to_map_packet(struct Character *chr, uint8_t *packet);

#define MOVE_PLAYER_PACKET_MAX_LENGTH 3863
size_t move_player_packet(uint32_t id, size_t len, uint8_t *data, uint8_t *packet);

#define DAMAGE_PLAYER_PACKET_MAX_LENGTH 28
size_t damange_player_packet(uint8_t skill, uint32_t monster_id, uint32_t char_id, int32_t damage, int32_t fake, uint8_t direction, uint8_t *packet);

#define REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH 6
void remove_player_from_map_packet(uint32_t id, uint8_t *packet);

#define SPAWN_NPC_PACKET_LENGTH 22
void spawn_npc_packet(uint32_t oid, uint32_t id, int16_t x, int16_t y, bool f, uint16_t fh, int16_t rx0, int16_t rx1, uint8_t *packet);

#define SPAWN_NPC_CONTROLLER_PACKET_LENGTH 23
void spawn_npc_controller_packet(uint32_t oid, uint32_t id, int16_t x, int16_t cy, bool f, uint16_t fh, int16_t rx0, int16_t rx1, uint8_t *packet);

#define SPAWN_MONSTER_PACKET_LENGTH 42
void spawn_monster_packet(uint32_t oid, uint32_t id, int16_t x, int16_t y, uint16_t fh, bool new, uint8_t *packet);

#define SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH 43
void spawn_monster_controller_packet(uint32_t oid, bool aggro, uint32_t id, int16_t x, int16_t y, uint16_t fh, bool new, uint8_t *packet);

#define REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH 7
void remove_monster_controller_packet(uint32_t oid, uint8_t *packet);

void npc_action_packet(size_t size, uint8_t *data, uint8_t *packet);

#define MOVE_MOB_PACKET_MAX_LENGTH 3843
size_t move_monster_packet(uint32_t oid, uint8_t activity, size_t len, uint8_t *data, uint8_t *packet);

#define MOVE_MOB_RESPONSE_PACKET_LENGTH 13
void move_monster_response_packet(uint32_t oid, uint16_t moveid, uint8_t *packet);

#define CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH 994
size_t close_range_attack_packet(uint32_t id, uint8_t skill, uint8_t skill_level, uint8_t mob_count, uint8_t hit_count, uint32_t *oids, int32_t *damage, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint8_t *packet);

#define RANGED_ATTACK_PACKET_MAX_LENGTH 994
size_t ranged_attack_packet(uint32_t id, uint8_t skill, uint8_t skill_level, uint8_t monster_count, uint8_t hit_count, uint32_t *oids, int32_t *damage, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t projectile, uint8_t *packet);

#define MONSTER_HP_PACKET_LENGTH 7
void monster_hp_packet(uint32_t oid, uint8_t hp, uint8_t *packet);

#define KILL_MONSTER_PACKET_LENGTH 8
void kill_monster_packet(uint32_t oid, bool animation, uint8_t *packet);

enum Stat {
    STAT_SKIN = 0x1,
    STAT_FACE = 0x2,
    STAT_HAIR = 0x4,
    STAT_LEVEL = 0x10,
    STAT_JOB = 0x20,
    STAT_STR = 0x40,
    STAT_DEX = 0x80,
    STAT_INT = 0x100,
    STAT_LUK = 0x200,
    STAT_HP = 0x400,
    STAT_MAX_HP = 0x800,
    STAT_MP = 0x1000,
    STAT_MAX_MP = 0x2000,
    STAT_AP = 0x4000,
    STAT_SP = 0x8000,
    STAT_EXP = 0x10000,
    STAT_FAME = 0x20000,
    STAT_MESO = 0x40000,
    STAT_PET = 0x180008,
    STAT_GACHA_EXP = 0x200000
};

union StatValue {
    uint8_t u8;
    int16_t i16;
    uint16_t u16;
    int32_t i32;
    uint32_t u32;
};

#define STAT_CHANGE_PACKET_MAX_LENGTH 57
size_t stat_change_packet(bool enable_actions, enum Stat stats, union StatValue *values, uint8_t *packet);

#define EXP_GAIN_PACKET_LENGTH 36
void exp_gain_packet(int32_t exp, int32_t equip_bonus, int32_t party_bonus, bool white, uint8_t *packet);

#define EXP_GAIN_IN_CHAT_PACKET_LENGTH 37
void exp_gain_in_chat_packet(int32_t exp, int32_t equip_bonus, int32_t party_bonus, bool white, uint8_t *packet);

#define SHOW_EFFECT_PACKET_LENGTH 3
void show_effect_packet(uint8_t effect, uint8_t *packet);

#define SHOW_FOREIGN_EFFECT_PACKET_LENGTH 7
void show_foreign_effect_packet(uint32_t id, uint8_t effect, uint8_t *packet);

#define DROP_ITEM_FROM_OBJECT_PACKET_LENGTH 40
void drop_item_from_object_packet(uint32_t oid, uint32_t item_id, uint32_t owner_id, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, uint32_t dropper_oid, uint8_t *packet);

#define SPAWN_ITEM_DROP_PACKET_LENGTH 34
void spawn_item_drop_packet(uint32_t oid, uint32_t item_id, uint32_t owner_id, int16_t to_x, int16_t to_y, uint32_t dropper_oid, uint8_t *packet);

#define REMOVE_DROP_PACKET_LENGTH 7
void remove_drop_packet(uint32_t oid, uint8_t *packet);

#define PICKUP_DROP_PACKET_LENGTH 11
void pickup_drop_packet(uint32_t oid, bool is_exploding, uint32_t char_id, uint8_t *packet);

#define PET_PICKUP_DROP_PACKET_LENGTH 12
void pet_pickup_drop_packet(uint32_t oid, bool is_exploding, uint32_t char_id, uint8_t pet, uint8_t *packet);

#define DROP_MESO_FROM_OBJECT_PACKET_LENGTH 32
void drop_meso_from_object_packet(uint32_t oid, uint32_t owner_id, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, uint32_t dropper_oid, uint8_t *packet);

#define SPAWN_MESO_DROP_PACKET_LENGTH 26
void spawn_meso_drop_packet(uint32_t oid, uint32_t owner_id, int16_t x, int16_t y, uint32_t dropper_oid, uint8_t *packet);

#define MESO_GAIN_PACKET_LENGTH 11
void meso_gain_packet(int32_t amount, uint8_t *packet);

#define MESO_GAIN_IN_CHAT_PACKET_LENGTH 9
void meso_gain_in_chat_packet(int32_t amount, uint8_t *packet);

#define ITEM_GAIN_PACKET_LENGTH 20
void item_gain_packet(uint32_t id, int32_t amount, uint8_t *packet);

#define ITEM_GAIN_IN_CHAT_PACKET_LENGTH 12
void item_gain_in_chat_packet(uint32_t id, int32_t amount, uint8_t *packet);

#define ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH 16
void item_unavailable_notification_packet(uint8_t *packet);

#define INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH 16
void inventory_full_notification_packet(uint8_t *packet);

#define START_QUEST_PACKET_LENGTH 13
void start_quest_packet(uint16_t qid, uint32_t npc, uint8_t *packet);

#define END_QUEST_PACKET_LENGTH 13
void end_quest_packet(uint16_t qid, uint32_t npc, uint16_t next, uint8_t *packet);

#define UPDATE_QUEST_PACKET_MAX_LENGTH 28
size_t update_quest_packet(uint16_t id, uint16_t len, char *progress, uint8_t *packet);

#define UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH 14
void update_quest_completion_time_packet(uint16_t qid, time_t time, uint8_t *packet);

#define FORFEIT_QUEST_PACKET_LENGTH 6
void forfeit_quest_packet(uint16_t qid, uint8_t *packet);

enum InventoryModifyType {
    INVENTORY_MODIFY_TYPE_ADD,
    INVENTORY_MODIFY_TYPE_MODIFY,
    INVENTORY_MODIFY_TYPE_MOVE,
    INVENTORY_MODIFY_TYPE_REMOVE
};

struct InventoryModify {
    enum InventoryModifyType mode;
    uint8_t inventory;
    int16_t slot;
    union {
        struct {
            union {
                struct InventoryItem item;
                struct Equipment equip;
            };
        };
        struct {
            int16_t quantity;
        };
        struct {
            int16_t dst;
        };
    };
};

#define MODIFY_ITEMS_PACKET_MAX_LENGTH 10204 // Probably more since this only concerns non-equipment items
size_t modify_items_packet(uint8_t mod_count, struct InventoryModify *mods, uint8_t *packet);

enum NpcDialogueType {
    NPC_DIALOGUE_TYPE_OK = 0,
    NPC_DIALOGUE_TYPE_YES_NO = 1,
    NPC_DIALOGUE_TYPE_SIMPLE = 4,
    NPC_DIALOGUE_TYPE_NEXT = 5,
    NPC_DIALOGUE_TYPE_PREV_NEXT = 6,
    NPC_DIALOGUE_TYPE_PREV = 7,
    NPC_DIALOGUE_TYPE_ACCEPT_DECILNE = 12
};

#define NPC_DIALOGUE_PACKET_MAX_LENGTH 65535
size_t npc_dialogue_packet(uint32_t npc, enum NpcDialogueType type, uint16_t message_len, const char *message, uint8_t *packet);

#define UPDATE_SKILL_PACKET_LENGTH 26
void update_skill_packet(uint32_t id, int8_t level, int8_t master_level, uint8_t *packet);

#define POPUP_MESSAGE_PACKET_MAX_LENGTH 132 // Assuming a 128-character limit
size_t popup_message_packet(uint16_t len, const char *message, uint8_t *packet);

#endif


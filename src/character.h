#ifndef CHARACTER_H
#define CHARACTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "item.h"
#include "job.h"

#define CHARACTER_MAX_NAME_LENGTH 12

#define MP_MAX 30000
#define HP_MAX 30000

enum CharacterGender {
    CHARACTER_GENDER_MALE,
    CHARACTER_GENDER_FEMALE,
};

// Structure used only for visual data about a character.
// Used in megaphone avatar, map avatar etc.
struct CharacterAppearance {
    uint8_t nameLength;
    char name[CHARACTER_MAX_NAME_LENGTH];
    //unsigned int accountId;
    bool gender; // false - male, true - female
    uint8_t skin;
    uint32_t face;
    uint32_t hair;

    // Stats
    int32_t gachaExp;
    uint32_t map;
    uint8_t spawnPoint;

    struct {
        bool isEmpty;
        uint32_t id;
    } equipments[EQUIP_SLOT_COUNT];
};

// Structure used for the character info card when you double click a character
struct CharacterInfo {
    struct CharacterAppearance appearance;
    uint8_t level;
    enum Job job;
    int16_t fame;
};

// Structure used for the character selection screen
struct CharacterStats {
    uint32_t id;
    struct CharacterInfo info;
    int16_t str;
    int16_t dex;
    int16_t int_;
    int16_t luk;
    int16_t maxHp;
    int16_t hp;
    int16_t maxMp;
    int16_t mp;
    int16_t ap;
    int16_t sp;
    int32_t exp;
};

#define MAX_ITEM_COUNT 252

struct Inventory {
    uint8_t slotCount;
    struct {
        bool isEmpty;
        struct InventoryItem item;
    } items[MAX_ITEM_COUNT];
};

struct QuestInfoProgress {
    uint16_t id;
    uint8_t length;
    char value[12];
};

struct Quest {
    uint16_t id;
    uint8_t progressCount;
    int32_t progress[5];
};

struct MonsterRefCount {
    uint32_t id;
    int8_t refCount;
};

struct CompletedQuest {
    uint16_t id;
    time_t time;
};

struct Skill {
    uint32_t id;
    int8_t level;
    int8_t masterLevel;
};

struct MonsterBookEntry {
    uint32_t id;
    int8_t count;
};

struct KeyMapEntry {
    uint8_t type;
    uint32_t action;
};

struct Storage {
    uint64_t id;
    uint8_t slots;
    uint8_t count;
    int32_t mesos;

    struct {
        bool isEquip;
        union {
            struct InventoryItem item;
            struct Equipment equip;
        };
    } storage[MAX_ITEM_COUNT];
};

#define KEYMAP_MAX_KEYS 90

size_t quest_get_progress_string(struct Quest *quest, char *out);

struct Character {
    uint32_t id;
    uint32_t accountId;
    uint8_t nameLength;
    char name[CHARACTER_MAX_NAME_LENGTH];
    uint32_t map;
    int16_t x, y;
    uint16_t fh;
    uint8_t stance;
    //unsigned int accountId;
    bool gender; // false - male, true - female
    uint8_t skin;
    uint32_t face;
    uint32_t hair;
    uint32_t chair;
    uint16_t seat;

    uint8_t level; // TODO: Maybe use a int16_t for faster calculations?
    enum Job job;
    int16_t fame;
    int16_t str;
    int16_t dex;
    int16_t int_;
    int16_t luk;
    int16_t hpmp;
    int16_t maxHp;
    int16_t hp;
    int16_t maxMp;
    int16_t mp;
    int16_t ap;
    int16_t sp;
    int32_t exp;
    int32_t estr; // e for extra, They are 4-byte as they can theoratically be much larger then INT16_MAX
    int32_t edex;
    int32_t eint;
    int32_t eluk;
    int32_t eMaxHp;
    int32_t eMaxMp;

    int32_t gachaExp;
    uint8_t spawnPoint;

    int32_t mesos;

    struct Storage storage;

    struct {
        bool isEmpty;
        struct Equipment equip;
    } equippedEquipment[EQUIP_SLOT_COUNT];

    struct {
        uint8_t slotCount;
        struct {
            bool isEmpty;
            struct Equipment equip;
        } items[MAX_ITEM_COUNT];
    } equipmentInventory;

    struct Inventory inventory[4]; // All other inventories - 0: Use, 1: Set-up 2: Etc. 3: Cash

    uint8_t activeProjectile;

    struct HashSetU16 *quests;
    // For each monster ID in this set, there is a reference count of the number of quests that have this monster as a requirement.
    // This is used in client_kill_monster() to quickly check if the monster has a quest before starting to iterate over the elements of \p quests
    struct HashSetU32 *monsterQuests;
    struct HashSetU32 *itemQuests;
    struct HashSetU16 *questInfos;
    struct HashSetU16 *completedQuests;

    struct HashSetU32 *skills;

    struct HashSetU32 *monsterBook;

    struct KeyMapEntry keyMap[KEYMAP_MAX_KEYS];
};

struct CharacterStats character_to_character_stats(const struct Character *chr);

struct CharacterAppearance character_to_character_appearance(const struct Character *chr);

void character_set_job(struct Character *chr, uint16_t job);
void character_set_max_hp(struct Character *chr, int16_t hp);
void character_set_hp(struct Character *chr, int16_t hp);
void character_set_max_mp(struct Character *chr, int16_t hp);
void character_set_mp(struct Character *chr, int16_t mp);
void character_set_str(struct Character *chr, int16_t str);
void character_set_dex(struct Character *chr, int16_t dex);
void character_set_int(struct Character *chr, int16_t int_);
void character_set_luk(struct Character *chr, int16_t luk);
void character_set_fame(struct Character *chr, int16_t fame);
void character_set_ap(struct Character *chr, int16_t ap);
void character_set_sp(struct Character *chr, int16_t sp);

/**
 * If \p exp is positive:
 *  * Returns a negative number if all the experience has been consumed
 *  * otherwise, A level up has occurred and the number of experience points that still need to be consumed will be returned
 *  * If 0 is returned then an 'exact' level up has occurred
 * otherwise:
 *  * Returns the number of experience points that have been deducted
 *  * Since a 'level-down' can't occur there is no need for an indication for it unlike the level-up case
 */
int32_t character_gain_exp(struct Character *chr, int32_t exp);
void character_set_meso(struct Character *chr, int32_t meso);
int16_t character_get_effective_str(struct Character *chr);
int16_t character_get_effective_dex(struct Character *chr);
int16_t character_get_effective_int(struct Character *chr);
int16_t character_get_effective_luk(struct Character *chr);
int16_t character_get_effective_hp(struct Character *chr);
int16_t character_get_effective_mp(struct Character *chr);

#endif


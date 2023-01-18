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

// TODO: Check if this is max 124 or 252
#define MAX_ITEM_COUNT 252

struct Inventory {
    uint8_t slotCount;
    struct {
        bool isEmpty;
        struct InventoryItem item;
    } items[MAX_ITEM_COUNT];
};

struct Progress {
    uint32_t id;
    int32_t amount;
};

struct Quest {
    uint16_t id;
    uint8_t progressCount;
    struct Progress progress[5];
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

size_t quest_get_progress_string(struct Quest *quest, char *out);

struct Character {
    uint32_t id;
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

    struct HashSetU16 *quests;
    // For each monster ID in this set, there is a reference count of the number of quests that have this monster as a requirement.
    // This is used in client_kill_monster() to quickly check if the monster has a quest before starting to iterate over the elements of \p quests
    struct HashSetU32 *monsterQuests; 
    struct HashSetU16 *completedQuests;

    struct HashSetU32 *skills;
};

struct CharacterStats character_to_character_stats(struct Character *chr);

struct CharacterAppearance character_to_character_appearance(struct Character *chr);

int16_t character_get_effective_str(struct Character *chr);
int16_t character_get_effective_dex(struct Character *chr);
int16_t character_get_effective_int(struct Character *chr);
int16_t character_get_effective_luk(struct Character *chr);
int16_t character_get_effective_hp(struct Character *chr);
int16_t character_get_effective_mp(struct Character *chr);

#endif


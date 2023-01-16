#ifndef WZ_H
#define WZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCRIPT_NAME_MAX_LENGTH 16

struct FootholdRTree;

struct Point {
    int16_t x;
    int16_t y;
};

struct Foothold {
    uint32_t id;
    struct Point p1;
    struct Point p2;
};

const struct Foothold *foothold_tree_find_below(const struct FootholdRTree *tree, struct Point *p);

enum LifeType {
    LIFE_TYPE_MOB,
    LIFE_TYPE_NPC,
    LIFE_TYPE_UNKNOWN
};

struct LifeInfo {
    enum LifeType type;
    uint32_t id;
    struct Point spawnPoint;
    uint16_t fh;
    int16_t cy;
    int16_t rx0;
    int16_t rx1;
    bool f;
};

#define REACTOR_INFO_NAME_MAX_LENGTH 16

struct ReactorInfo {
    uint32_t id;
    struct Point spawnPoint;
    int16_t reactorTime;
    bool f;
    char name[REACTOR_INFO_NAME_MAX_LENGTH+1];
};

#define PORTAL_INFO_NAME_MAX_LENGTH 16

enum PortalType {
    PORTAL_TYPE_REGULAR
};

struct PortalInfo {
    uint8_t id;
    enum PortalType type;
    char name[PORTAL_INFO_NAME_MAX_LENGTH+1];
    int16_t x;
    int16_t y;
    uint32_t targetMap;
    char targetName[PORTAL_INFO_NAME_MAX_LENGTH+1];
    char script[SCRIPT_NAME_MAX_LENGTH+1];

};

struct MapInfo {
    uint32_t id;
    float mobRate;
    uint32_t returnMapId;
    bool swim;
    char onFirstUserEnter[SCRIPT_NAME_MAX_LENGTH+1];
    char onUserEnter[SCRIPT_NAME_MAX_LENGTH+1];
    struct FootholdRTree *footholdTree;
    size_t lifeCount;
    struct LifeInfo *lives;
    size_t reactorCount;
    struct ReactorInfo *reactors;
    size_t portalCount;
    struct PortalInfo *portals;
};

struct MobInfo {
    uint32_t id;
    bool bodyAttack;
    uint8_t level;
    int32_t hp;
    int32_t mp;
    int8_t speed;
    int16_t atk;
    int16_t def;
    int16_t matk;
    int16_t mdef;
    int16_t acc;
    int16_t avoid;
    int32_t exp;
    bool undead;
    bool boss;
};

enum QuestRequirementType {
    QUEST_REQUIREMENT_TYPE_NPC,
    QUEST_REQUIREMENT_TYPE_QUEST,
    QUEST_REQUIREMENT_TYPE_JOB,
    QUEST_REQUIREMENT_TYPE_MIN_LEVEL,
    QUEST_REQUIREMENT_TYPE_MAX_LEVEL,
    QUEST_REQUIREMENT_TYPE_FIELD_ENTER,
    QUEST_REQUIREMENT_TYPE_INTERVAL,
    QUEST_REQUIREMENT_TYPE_START,
    QUEST_REQUIREMENT_TYPE_END,
    QUEST_REQUIREMENT_TYPE_PET,
    QUEST_REQUIREMENT_TYPE_MONSTER_BOOK,
    QUEST_REQUIREMENT_TYPE_COMPLETED_QUEST,
    QUEST_REQUIREMENT_TYPE_MESO,
    QUEST_REQUIREMENT_TYPE_BUFF,
    QUEST_REQUIREMENT_TYPE_EXCEPT_BUFF,
    QUEST_REQUIREMENT_TYPE_MOB,
    QUEST_REQUIREMENT_TYPE_ITEM
};

enum QuestState {
    QUEST_STATE_NOT_STARTED,
    QUEST_STATE_STARTED,
    QUEST_STATE_COMPLETED
};

struct QuestRequirement {
    enum QuestRequirementType type;
    union {
        struct {
            uint32_t id;
        } npc;
        struct {
            uint16_t id;
            enum QuestState state;
        } quest;
        struct {
            uint32_t id;
            int32_t count;
        } item;
        // The mob's requirement index is important
        struct {
            size_t count;
            struct {
                uint32_t id;
                int16_t count;
            } *mobs;
        } mob;
        struct {
            size_t count;
            uint16_t *jobs;
        } job;
        struct {
            uint8_t level;
        } minLevel, maxLevel;
        struct {
            int32_t amount;
        } meso, questCompleted;
        struct {
            int16_t hours;
        } interval;
    };
};

enum QuestActType {
    QUEST_ACT_TYPE_MESO,
    QUEST_ACT_TYPE_EXP,
    QUEST_ACT_TYPE_ITEM,
    QUEST_ACT_TYPE_QUEST,
    QUEST_ACT_TYPE_NEXT_QUEST,
    QUEST_ACT_TYPE_NPC,
    QUEST_ACT_TYPE_MIN_LEVEL,
    QUEST_ACT_TYPE_MAX_LEVEL,
    QUEST_ACT_TYPE_FAME,
    QUEST_ACT_TYPE_PET_SPEED,
    QUEST_ACT_TYPE_PET_TAMENESS,
    QUEST_ACT_TYPE_PET_SKILL,
    QUEST_ACT_TYPE_SKILL
};

struct QuestItemAction {
    uint32_t id;
    int16_t count;
    uint8_t gender;
    int16_t period;
    int32_t prop;
    uint8_t var; // What is this?
    uint16_t job;
};

struct QuestAct {
    enum QuestActType type;
    struct {
        int32_t amount;
    } meso, exp;
    struct {
        int16_t amount;
    } fame;
    struct {
        size_t count;
        struct QuestItemAction *items;
    } item;
    struct {
        uint16_t qid;
        enum QuestState state;
    } quest;
    struct {
        uint16_t qid;
    } nextQuest;
};

struct QuestInfo {
    uint16_t id;
    size_t startRequirementCount;
    struct QuestRequirement *startRequirements;
    size_t endRequirementCount;
    struct QuestRequirement *endRequirements;
    bool startScript;
    bool endScript;
    size_t startActCount;
    struct QuestAct *startActs;
    size_t endActCount;
    struct QuestAct *endActs;
};

struct ItemInfo {
    uint32_t id;
    int16_t slotMax;
};

struct EquipInfo {
    uint32_t id;
    uint16_t reqJob;
    uint8_t reqLevel;
    int16_t reqStr;
    int16_t reqDex;
    int16_t reqInt;
    int16_t reqLuk;
    int16_t str;
    int16_t dex;
    int16_t int_;
    int16_t luk;
    int16_t hp;
    int16_t mp;
    int16_t atk;
    int16_t matk;
    int16_t def;
    int16_t mdef;
    int16_t acc;
    int16_t avoid;
    int16_t speed;
    int16_t jump;
    int8_t slots;
    uint8_t attackSpeed;
    bool cash;
    // TODO: Level info
};

struct ConsumableInfo {
    uint32_t id;
    int16_t hp;
    int16_t mp;
    uint8_t hpR;
    uint8_t mpR;
    int16_t atk;
    int16_t matk;
    int16_t def;
    int16_t mdef;
    int16_t acc;
    int16_t avoid;
    int16_t speed;
    int16_t jump;
    int32_t time;
    // And more to come!
};

int wz_init();
int wz_init_equipment();
void wz_terminate();
void wz_terminate_equipment();
uint32_t wz_get_target_map(uint32_t id, char *target);
uint8_t wz_get_target_portal(uint32_t id, char *target);
const struct FootholdRTree *wz_get_foothold_tree_for_map(uint32_t id);
const struct LifeInfo *wz_get_life_for_map(uint32_t id, size_t *count);
const struct LifeInfo *wz_get_npcs_for_map(uint32_t id, size_t *count);
const struct LifeInfo *wz_get_mobs_for_map(uint32_t id, size_t *count);
const struct PortalInfo *wz_get_portal_info(uint32_t id, uint8_t pid);
const struct PortalInfo *wz_get_portal_info_by_name(uint32_t id, const char *name);
const struct EquipInfo *wz_get_equip_info(uint32_t id);
const struct ConsumableInfo *wz_get_consumable_info(uint32_t id);
const struct MobInfo *wz_get_monster_stats(uint32_t id);
const struct QuestInfo *wz_get_quest_info(uint16_t id);
const struct ItemInfo *wz_get_item_info(uint32_t id);

#endif


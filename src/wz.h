#ifndef WZ_H
#define WZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCRIPT_NAME_MAX_LENGTH 32

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

struct MapReactorInfo {
    uint32_t id;
    int16_t reactorTime;
    struct Point pos;
    bool f;
    // char name[REACTOR_INFO_NAME_MAX_LENGTH+1]; // Do I even need this?
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
    uint32_t returnMap;
    bool swim;
    char onFirstUserEnter[SCRIPT_NAME_MAX_LENGTH+1];
    char onUserEnter[SCRIPT_NAME_MAX_LENGTH+1];
    struct FootholdRTree *footholdTree;
    uint16_t seats;
    size_t lifeCount;
    struct LifeInfo *lives;
    size_t reactorCount;
    struct MapReactorInfo *reactors;
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
    QUEST_REQUIREMENT_TYPE_ITEM,
    QUEST_REQUIREMENT_TYPE_INFO
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
        struct {
            uint16_t number;
            size_t infoCount;
            char **infos;
        } info;
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

struct QuestSkillAction {
    uint32_t id;
    int8_t level;
    int8_t masterLevel;
    size_t jobCount;
    uint16_t *jobs;
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
    struct {
        size_t count;
        struct QuestSkillAction *skills;
    } skill;
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
    int32_t price;
    double unitPrice;
    bool untradable;
    bool oneOfAKind;
    bool monsterBook;
    bool quest;
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
    bool consumeOnPickup;
    // And more to come!
};

enum ReactorEventType {
    REACTOR_EVENT_TYPE_HIT = 0,
    REACTOR_EVENT_TYPE_HIT_FROM_LEFT = 1, // Probably, not sure
    REACTOR_EVENT_TYPE_HIT_FROM_RIGHT = 2, // Like the flowers in kerning's swamp
    REACTOR_EVENT_TYPE_SKILL = 5,
    REACTOR_EVENT_TYPE_TOUCH = 6,
    REACTOR_EVENT_TYPE_UNTOUCH = 7,
    REACTOR_EVENT_TYPE_ITEM = 100,
};

struct ReactorEventInfo {
    enum ReactorEventType type;
    uint8_t next;
    union {
        struct {
            uint32_t item;
            int16_t count;
            struct Point lt;
            struct Point rb;
        };
        struct {
            size_t skillCount;
            uint32_t *skills;
        };
    };
};
struct ReactorStateInfo {
    size_t eventCount;
    struct ReactorEventInfo *events;
};

struct ReactorInfo {
    uint32_t id;
    size_t stateCount;
    struct ReactorStateInfo *states;
    char action[SCRIPT_NAME_MAX_LENGTH];
};

struct SkillRequirementInfo {
    uint32_t id;
    int16_t level;
};

struct SkillLevelInfo {
    int16_t mpCon;
    int16_t time;
    int16_t damage;
    int16_t bulletCount;
};

struct SkillInfo {
    uint32_t id;
    size_t levelCount;
    struct SkillLevelInfo *levels;
    size_t reqCount; // Maybe each skill has only 1 pre-requisite?
    struct SkillRequirementInfo *reqs;
};

int wz_init(void);
int wz_init_equipment(void);
void wz_terminate(void);
void wz_terminate_equipment(void);
uint32_t wz_get_map_nearest_town(uint32_t id);
uint16_t wz_get_map_seat_count(uint32_t id);
uint32_t wz_get_target_map(uint32_t id, char *target);
uint8_t wz_get_target_portal(uint32_t id, char *target);
const struct FootholdRTree *wz_get_foothold_tree_for_map(uint32_t id);
const struct LifeInfo *wz_get_life_for_map(uint32_t id, size_t *count);
const struct MapReactorInfo *wz_get_reactors_for_map(uint32_t id, size_t *count);
const struct PortalInfo *wz_get_portal_info(uint32_t id, uint8_t pid);
const struct PortalInfo *wz_get_portal_info_by_name(uint32_t id, const char *name);
const struct EquipInfo *wz_get_equip_info(uint32_t id);
const struct ConsumableInfo *wz_get_consumable_info(uint32_t id);
const struct MobInfo *wz_get_monster_stats(uint32_t id);
const struct QuestInfo *wz_get_quest_info(uint16_t id);
const struct ItemInfo *wz_get_item_info(uint32_t id);
const struct ReactorInfo *wz_get_reactor_info(uint32_t id);
const struct SkillInfo *wz_get_skill_info(uint32_t id);

#endif


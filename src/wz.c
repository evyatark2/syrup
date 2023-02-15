#include "wz.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h> // mmap
#include <sys/stat.h>
#include <sys/types.h> // opendir, fdopendir, dirfd, readdir, closedir
#include <dirent.h> // opendir, fdopendir, dirfd, readdir, closedir
#include <semaphore.h> // sem_open, sem_wait, sem_post
#include <unistd.h> // ftruncate

#include <expat.h>

#include <cmph.h>

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

struct Rectangle {
    struct Point sw;
    struct Point ne;
};

struct RTreeNode {
    struct RTreeNode *parent;
    struct Rectangle bound;
    bool isLeaf;
    uint8_t count;
    union {
        struct RTreeNode *children[3];
        struct Foothold footholds[3];
    };
};

struct FootholdRTree {
    struct RTreeNode *root;
};

static void insert_foothold(struct FootholdRTree *tree, struct Foothold *fh);

bool WRITER;

cmph_t *MAP_INFO_MPH;
static size_t MAP_INFO_COUNT;
static struct MapInfo *MAP_INFOS;

cmph_t *MOB_INFO_MPH;
static size_t MOB_INFO_COUNT;
static struct MobInfo *MOB_INFOS;

cmph_t *QUEST_INFO_MPH;
static size_t QUEST_INFO_COUNT;
static struct QuestInfo *QUEST_INFOS;

cmph_t *ITEM_INFO_MPH;
static size_t ITEM_INFO_COUNT;
static struct ItemInfo *ITEM_INFOS;

cmph_t *EQUIP_INFO_MPH;
static size_t EQUIP_INFO_COUNT;
static struct EquipInfo *EQUIP_INFOS;

cmph_t *CONSUMABLE_INFO_MPH;
static size_t CONSUMABLE_INFO_COUNT;
static struct ConsumableInfo *CONSUMABLE_INFOS;

cmph_t *REACTOR_INFO_MPH;
static size_t REACTOR_INFO_COUNT;
static struct ReactorInfo *REACTOR_INFOS;

enum MapItemType {
    MAP_ITEM_TYPE_TOP_LEVEL,
    MAP_ITEM_TYPE_INFO,
    MAP_ITEM_TYPE_FOOTHOLDS,
    MAP_ITEM_TYPE_PORTALS,
    MAP_ITEM_TYPE_PORTAL,
    MAP_ITEM_TYPE_LIVES,
    MAP_ITEM_TYPE_LIFE,
    MAP_ITEM_TYPE_REACTORS,
    MAP_ITEM_TYPE_REACTOR
};

struct MapParserStackNode {
    struct MapParserStackNode *next;
    enum MapItemType type;
};

struct MapParserContext {
    struct MapParserStackNode *head;
    uint32_t currentMap;
    uint32_t currentLife;
    uint32_t currentPortal;
    uint32_t reactorCapacity;
    uint32_t skip;
    uint8_t footholdLevel;
    struct Foothold currentFoothold;
};

enum MobItemType {
    MOB_ITEM_TYPE_TOP_LEVEL,
    MOB_ITEM_TYPE_INFO,
    MOB_ITEM_TYPE_SKILLS,
    MOB_ITEM_TYPE_SKILL
};

struct MobParserStackNode {
    struct MobParserStackNode *next;
    enum MobItemType type;
};

struct MobParserContext {
    XML_Parser parser;
    struct MobParserStackNode *head;
    //uint32_t currentMob;
    uint32_t currentSkill;
    uint32_t skip;
};

enum QuestCheckItemType {
    QUEST_CHECK_ITEM_TYPE_TOP_LEVEL,
    QUEST_CHECK_ITEM_TYPE_QUEST,
    QUEST_CHECK_ITEM_TYPE_START,
    QUEST_CHECK_ITEM_TYPE_END,
    QUEST_CHECK_ITEM_TYPE_REQ_JOB,
    QUEST_CHECK_ITEM_TYPE_REQ_QUESTS,
    QUEST_CHECK_ITEM_TYPE_REQ_QUEST,
    QUEST_CHECK_ITEM_TYPE_REQ_ITEMS,
    QUEST_CHECK_ITEM_TYPE_REQ_ITEM,
    QUEST_CHECK_ITEM_TYPE_REQ_MOBS,
    QUEST_CHECK_ITEM_TYPE_REQ_MOB,
    QUEST_CHECK_ITEM_TYPE_REQ_INFOS,
    QUEST_CHECK_ITEM_TYPE_REQ_INFO,
};

struct QuestCheckParserStackNode {
    struct QuestCheckParserStackNode *next;
    enum QuestCheckItemType type;
};

struct QuestCheckParserContext {
    struct QuestCheckParserStackNode *head;
    size_t questCapacity;
    size_t reqCapacity;
    size_t capacity;
    struct QuestRequirement *infoReq;
    uint32_t skip;
};

enum QuestActItemType {
    QUEST_ACT_ITEM_TYPE_TOP_LEVEL,
    QUEST_ACT_ITEM_TYPE_QUEST,
    QUEST_ACT_ITEM_TYPE_START,
    QUEST_ACT_ITEM_TYPE_END,
    QUEST_ACT_ITEM_TYPE_ACT_QUESTS,
    QUEST_ACT_ITEM_TYPE_ACT_QUEST,
    QUEST_ACT_ITEM_TYPE_ACT_ITEMS,
    QUEST_ACT_ITEM_TYPE_ACT_ITEM,
};

struct QuestActParserStackNode {
    struct QuestActParserStackNode *next;
    enum QuestActItemType type;
};

struct QuestActParserContext {
    struct QuestActParserStackNode *head;
    size_t currentQuest;
    size_t questCapacity;
    size_t actCapacity;
    size_t itemCapacity;
    uint32_t skip;
};

enum ItemItemType {
    ITEM_ITEM_TYPE_TOP_LEVEL,
    ITEM_ITEM_TYPE_ITEM,
    ITEM_ITEM_TYPE_INFO,
};

struct ItemParserStackNode {
    struct ItemParserStackNode *next;
    enum ItemItemType type;
};

enum EquipItemItemType {
    EQUIP_ITEM_ITEM_TYPE_TOP_LEVEL,
    EQUIP_ITEM_ITEM_TYPE_INFO,
};

struct EquipItemParserStackNode {
    struct EquipItemParserStackNode *next;
    enum EquipItemItemType type;
};

struct ItemParserContext {
    XML_Parser parser;
    union {
        struct ItemParserStackNode *head;
        struct EquipItemParserStackNode *head2;
    };
    size_t itemCapacity;
    uint32_t skip;
};

enum EquipItemType {
    EQUIP_ITEM_TYPE_TOP_LEVEL,
    EQUIP_ITEM_TYPE_INFO,
    EQUIP_ITEM_TYPE_LEVEL,
};

struct EquipParserStackNode {
    struct EquipParserStackNode *next;
    enum EquipItemType type;
};

struct EquipParserContext {
    XML_Parser parser;
    struct EquipParserStackNode *head;
    uint32_t currentEquip;
    uint32_t skip;
};

enum ConsumableItemType {
    CONSUMABLE_ITEM_TYPE_TOP_LEVEL,
    CONSUMABLE_ITEM_TYPE_ITEM,
    CONSUMABLE_ITEM_TYPE_SPEC,
};

struct ConsumableParserStackNode {
    struct ConsumableParserStackNode *next;
    enum ConsumableItemType type;
};

struct ConsumableParserContext {
    struct ConsumableParserStackNode *head;
    size_t itemCapacity;
    uint32_t skip;
};

enum ReactorItemType {
    REACTOR_ITEM_TYPE_TOP_LEVEL,
    REACTOR_ITEM_TYPE_INFO,
    REACTOR_ITEM_TYPE_STATE,
    REACTOR_ITEM_TYPE_EVENTS,
    REACTOR_ITEM_TYPE_EVENT,
    REACTOR_ITEM_TYPE_SKILLS,
};

struct ReactorParserStackNode {
    struct ReactorParserStackNode *next;
    enum ReactorItemType type;
};

struct ReactorParserContext {
    XML_Parser parser;
    struct ReactorParserStackNode *head;
    size_t stateCapacity;
    size_t eventCapacity;
    size_t skillCapacity;
    uint32_t skip;
    bool isLink;
};

static void on_map_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_map_end(void *user_data, const XML_Char *name);
static void on_mob_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_mob_end(void *user_data, const XML_Char *name);
static void on_quest_check_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_quest_check_end(void *user_data, const XML_Char *name);
static void on_quest_act_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_quest_act_end(void *user_data, const XML_Char *name);
static void on_item_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_item_end(void *user_data, const XML_Char *name);
static void on_equip_item_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_equip_item_end(void *user_data, const XML_Char *name);
static void on_equip_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_equip_end(void *user_data, const XML_Char *name);
static void on_consumable_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_consumable_end(void *user_data, const XML_Char *name);
static void on_reactor_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_reactor_end(void *user_data, const XML_Char *name);
static void on_reactor_second_pass_start(void *user_data, const XML_Char *name, const XML_Char **attrs);
static void on_reactor_second_pass_end(void *user_data, const XML_Char *name);

int wz_init(void)
{
    //int shm;
    //sem_t *sem;
    //while (true) {
    //    if ((sem = sem_open("/syrup-wz", O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 0)) == SEM_FAILED) {
    //        if (errno == EEXIST) {
    //            if ((sem = sem_open("/syrup-wz", O_RDWR)) == SEM_FAILED) {
    //                if (errno != ENONET) {
    //                    return -1;
    //                }
    //            } else {
    //                sem_wait(sem);
    //                sem_post(sem);
    //                shm = shm_open("/syrup-wz", O_RDONLY, 0);
    //                break;
    //            }
    //        } else {
    //            return -1;
    //        }
    //    } else {
    //        shm = shm_open("/syrup-wz", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    //        WRITER = true;
    //        break;
    //    }
    //}

    WRITER = true;
    if (WRITER) {
        XML_Parser parser = XML_ParserCreate(NULL);

        {
            struct ReactorParserContext ctx = {
                .parser = parser,
                .head = NULL,
            };
            DIR *reactor_dir = opendir("wz/Reactor.wz");
            struct dirent *entry;
            size_t count = 0;
            while ((entry = readdir(reactor_dir)) != NULL) {
                if (entry->d_name[0] != '.' && entry->d_type == DT_REG)
                    count++;
            }

            REACTOR_INFOS = malloc(count * sizeof(struct ReactorInfo));
            rewinddir(reactor_dir);

            // First pass - unlinked reactors
            while ((entry = readdir(reactor_dir)) != NULL) {
                if (entry->d_name[0] == '.' || entry->d_type != DT_REG)
                    continue;
                int fd = openat(dirfd(reactor_dir), entry->d_name, O_RDONLY);
                off_t len = lseek(fd, 0, SEEK_END);
                lseek(fd, 0, SEEK_SET);
                char *data = malloc(len);
                read(fd, data, len);
                close(fd);

                XML_SetElementHandler(parser, on_reactor_start, on_reactor_end);
                XML_SetUserData(parser, &ctx);
                XML_Parse(parser, data, len, true);
                free(data);
                XML_ParserReset(parser, NULL);
            }
            //closedir(reactor_dir);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(REACTOR_INFOS, sizeof(struct ReactorInfo), offsetof(struct ReactorInfo, id), sizeof(uint32_t), REACTOR_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            REACTOR_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < REACTOR_INFO_COUNT) {
                uint32_t j = cmph_search(REACTOR_INFO_MPH, (void *)&REACTOR_INFOS[i].id, sizeof(uint32_t));
                if (i != j) {
                    struct ReactorInfo temp = REACTOR_INFOS[j];
                    REACTOR_INFOS[j] = REACTOR_INFOS[i];
                    REACTOR_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            rewinddir(reactor_dir);

            // Second pass - linked reactors
            while ((entry = readdir(reactor_dir)) != NULL) {
                if (entry->d_name[0] == '.' || entry->d_type != DT_REG)
                    continue;
                int fd = openat(dirfd(reactor_dir), entry->d_name, O_RDONLY);
                off_t len = lseek(fd, 0, SEEK_END);
                lseek(fd, 0, SEEK_SET);
                char *data = malloc(len);
                read(fd, data, len);
                close(fd);

                XML_SetElementHandler(parser, on_reactor_second_pass_start, on_reactor_second_pass_end);
                XML_SetUserData(parser, &ctx);
                XML_Parse(parser, data, len, true);
                free(data);
                if (REACTOR_INFO_COUNT == count) {
                    XML_ParserReset(parser, NULL);
                    break;
                }
                XML_ParserReset(parser, NULL);
            }
            closedir(reactor_dir);
            cmph_destroy(REACTOR_INFO_MPH);

            adapter = cmph_io_struct_vector_adapter(REACTOR_INFOS, sizeof(struct ReactorInfo), offsetof(struct ReactorInfo, id), sizeof(uint32_t), REACTOR_INFO_COUNT);
            config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            REACTOR_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            i = 0;
            while (i < REACTOR_INFO_COUNT) {
                uint32_t j = cmph_search(REACTOR_INFO_MPH, (void *)&REACTOR_INFOS[i].id, sizeof(uint32_t));
                if (i != j) {
                    struct ReactorInfo temp = REACTOR_INFOS[j];
                    REACTOR_INFOS[j] = REACTOR_INFOS[i];
                    REACTOR_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            fprintf(stderr, "Loaded reactors\n");
        }

        {
            struct EquipParserContext ctx = {
                .parser = parser,
                .head = NULL,
                .currentEquip = 0,
            };
            DIR *equip_dir = opendir("wz/Character.wz");
            struct dirent *entry;
            while ((entry = readdir(equip_dir)) != NULL) {
                if (entry->d_type == DT_DIR && entry->d_name[0] != '.' && strcmp(entry->d_name, "Afterimage")) {
                    int fd = openat(dirfd(equip_dir), entry->d_name, O_RDONLY | O_DIRECTORY);
                    DIR *dir = fdopendir(fd);
                    while ((entry = readdir(dir)) != NULL) {
                        if (entry->d_name[0] != '.')
                            EQUIP_INFO_COUNT++;
                    }
                    closedir(dir);
                }
            }

            EQUIP_INFOS = malloc(EQUIP_INFO_COUNT * sizeof(struct EquipInfo));
            rewinddir(equip_dir);

            while ((entry = readdir(equip_dir)) != NULL) {
                if (entry->d_type == DT_DIR && entry->d_name[0] != '.' && strcmp(entry->d_name, "Afterimage")) {
                    int fd = openat(dirfd(equip_dir), entry->d_name, O_RDONLY | O_DIRECTORY);
                    DIR *dir = fdopendir(fd);
                    while ((entry = readdir(dir)) != NULL) {
                        if (entry->d_name[0] == '.')
                            continue;
                        fd = openat(dirfd(dir), entry->d_name, O_RDONLY);
                        off_t len = lseek(fd, 0, SEEK_END);
                        lseek(fd, 0, SEEK_SET);
                        char *data = malloc(len);
                        read(fd, data, len);
                        close(fd);

                        XML_SetElementHandler(parser, on_equip_start, on_equip_end);
                        XML_SetUserData(parser, &ctx);
                        XML_Parse(parser, data, len, true);
                        free(data);
                        XML_ParserReset(parser, NULL);
                        ctx.currentEquip++;
                    }
                    closedir(dir);
                }
            }
            closedir(equip_dir);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(EQUIP_INFOS, sizeof(struct EquipInfo), offsetof(struct EquipInfo, id), sizeof(uint32_t), EQUIP_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            EQUIP_INFO_MPH = cmph_new(config);
            assert(EQUIP_INFO_MPH != NULL);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < EQUIP_INFO_COUNT) {
                uint32_t j = cmph_search(EQUIP_INFO_MPH, (void *)&EQUIP_INFOS[i].id, sizeof(uint32_t));
                if (i != j) {
                    struct EquipInfo temp = EQUIP_INFOS[j];
                    EQUIP_INFOS[j] = EQUIP_INFOS[i];
                    EQUIP_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            fprintf(stderr, "Loaded equipment\n");
        }

        {
            struct QuestCheckParserContext ctx = {
                .questCapacity = 1,
            };

            QUEST_INFOS = malloc(sizeof(struct QuestInfo));

            XML_SetElementHandler(parser, on_quest_check_start, on_quest_check_end);
            XML_SetUserData(parser, &ctx);
            int fd = open("wz/Quest.wz/Check.img.xml", O_RDONLY);
            off_t len = lseek(fd, 0, SEEK_END);
            lseek(fd, 0, SEEK_SET);
            char *data = malloc(len);
            read(fd, data, len);
            close(fd);
            XML_Parse(parser, data, len, true);
            free(data);
            XML_ParserReset(parser, NULL);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(QUEST_INFOS, sizeof(struct QuestInfo), offsetof(struct QuestInfo, id), sizeof(uint16_t), QUEST_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            QUEST_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < QUEST_INFO_COUNT) {
                uint32_t j = cmph_search(QUEST_INFO_MPH, (void *)&QUEST_INFOS[i].id, sizeof(uint16_t));
                if (i != j) {
                    struct QuestInfo temp = QUEST_INFOS[j];
                    QUEST_INFOS[j] = QUEST_INFOS[i];
                    QUEST_INFOS[i] = temp;
                } else {
                    i++;
                }
            }
        }

        {
            struct QuestActParserContext ctx = {
                .questCapacity = QUEST_INFO_COUNT,
            };

            XML_SetElementHandler(parser, on_quest_act_start, on_quest_act_end);
            XML_SetUserData(parser, &ctx);
            int fd = open("wz/Quest.wz/Act.img.xml", O_RDONLY);
            off_t len = lseek(fd, 0, SEEK_END);
            lseek(fd, 0, SEEK_SET);
            char *data = malloc(len);
            read(fd, data, len);
            close(fd);
            XML_Parse(parser, data, len, true);
            free(data);
            XML_ParserReset(parser, NULL);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(QUEST_INFOS, sizeof(struct QuestInfo), offsetof(struct QuestInfo, id), sizeof(uint16_t), QUEST_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            cmph_destroy(QUEST_INFO_MPH);
            QUEST_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < QUEST_INFO_COUNT) {
                uint32_t j = cmph_search(QUEST_INFO_MPH, (void *)&QUEST_INFOS[i].id, sizeof(uint16_t));
                if (i != j) {
                    struct QuestInfo temp = QUEST_INFOS[j];
                    QUEST_INFOS[j] = QUEST_INFOS[i];
                    QUEST_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            fprintf(stderr, "Loaded quests\n");
        }

        {
            struct MobParserContext ctx = {
                .parser = parser,
                .head = NULL,
            };
            DIR *mobs_dir = opendir("wz/Mob.wz");
            struct dirent *entry;
            size_t count = 0;
            while ((entry = readdir(mobs_dir)) != NULL) {
                if (entry->d_name[0] != '.' && entry->d_type == DT_REG)
                    count++;
            }

            MOB_INFOS = malloc(count * sizeof(struct MobInfo));
            rewinddir(mobs_dir);

            while ((entry = readdir(mobs_dir)) != NULL) {
                if (entry->d_name[0] == '.' || entry->d_type != DT_REG)
                    continue;
                int fd = openat(dirfd(mobs_dir), entry->d_name, O_RDONLY);
                off_t len = lseek(fd, 0, SEEK_END);
                lseek(fd, 0, SEEK_SET);
                char *data = malloc(len);
                read(fd, data, len);
                close(fd);

                //ctx.currentSkill = 1;
                //MOB_INFOS[ctx.currentMob].lifeCount = 0;
                //MOB_INFOS[ctx.currentMob].lives = malloc(sizeof(struct LifeInfo));

                XML_SetElementHandler(parser, on_mob_start, on_mob_end);
                XML_SetUserData(parser, &ctx);
                XML_Parse(parser, data, len, true);
                free(data);
                XML_ParserReset(parser, NULL);
                MOB_INFO_COUNT++;
            }
            closedir(mobs_dir);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(MOB_INFOS, sizeof(struct MobInfo), offsetof(struct MobInfo, id), sizeof(uint32_t), MOB_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            MOB_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < MOB_INFO_COUNT) {
                uint32_t j = cmph_search(MOB_INFO_MPH, (void *)&MOB_INFOS[i].id, sizeof(uint32_t));
                if (i != j) {
                    struct MobInfo temp = MOB_INFOS[j];
                    MOB_INFOS[j] = MOB_INFOS[i];
                    MOB_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            fprintf(stderr, "Loaded mobs\n");
        }

        {
            struct MapParserContext ctx = {
                .head = NULL,
            };
            DIR *maps_dir = opendir("wz/Map.wz/Map");
            struct dirent *entry;
            while ((entry = readdir(maps_dir)) != NULL) {
                if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
                    int fd = openat(dirfd(maps_dir), entry->d_name, O_RDONLY | O_DIRECTORY);
                    DIR *dir = fdopendir(fd);
                    while ((entry = readdir(dir)) != NULL) {
                        if (entry->d_name[0] != '.')
                            MAP_INFO_COUNT++;
                    }
                    closedir(dir);
                }
            }

            MAP_INFOS = malloc(MAP_INFO_COUNT * sizeof(struct MapInfo));
            for (size_t i = 0; i < MAP_INFO_COUNT; i++) {
                MAP_INFOS[i].footholdTree = malloc(sizeof(struct FootholdRTree));
                MAP_INFOS[i].footholdTree->root = NULL;
            }
            rewinddir(maps_dir);

            while ((entry = readdir(maps_dir)) != NULL) {
                if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
                    int fd = openat(dirfd(maps_dir), entry->d_name, O_RDONLY | O_DIRECTORY);
                    DIR *dir = fdopendir(fd);
                    while ((entry = readdir(dir)) != NULL) {
                        if (entry->d_name[0] == '.')
                            continue;
                        fd = openat(dirfd(dir), entry->d_name, O_RDONLY);
                        off_t len = lseek(fd, 0, SEEK_END);
                        lseek(fd, 0, SEEK_SET);
                        char *data = malloc(len);
                        read(fd, data, len);
                        close(fd);

                        ctx.currentLife = 1;
                        MAP_INFOS[ctx.currentMap].lifeCount = 0;
                        MAP_INFOS[ctx.currentMap].lives = malloc(sizeof(struct LifeInfo));

                        ctx.currentPortal = 1;
                        MAP_INFOS[ctx.currentMap].portalCount = 0;
                        MAP_INFOS[ctx.currentMap].portals = malloc(sizeof(struct PortalInfo));

                        ctx.reactorCapacity = 1;
                        MAP_INFOS[ctx.currentMap].reactorCount = 0;
                        MAP_INFOS[ctx.currentMap].reactors = malloc(sizeof(struct MapReactorInfo));

                        XML_SetElementHandler(parser, on_map_start, on_map_end);
                        XML_SetUserData(parser, &ctx);
                        XML_Parse(parser, data, len, true);
                        free(data);
                        XML_ParserReset(parser, NULL);
                        ctx.currentMap++;
                    }
                    closedir(dir);
                }
            }
            closedir(maps_dir);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(MAP_INFOS, sizeof(struct MapInfo), offsetof(struct MapInfo, id), sizeof(uint32_t), MAP_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            MAP_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < MAP_INFO_COUNT) {
                uint32_t j = cmph_search(MAP_INFO_MPH, (void *)&MAP_INFOS[i].id, sizeof(uint32_t));
                if (i != j) {
                    struct MapInfo temp = MAP_INFOS[j];
                    MAP_INFOS[j] = MAP_INFOS[i];
                    MAP_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            fprintf(stderr, "Loaded maps\n");
        }

        {
            struct ItemParserContext ctx = {
                .parser = parser,
                .head = NULL,
                .itemCapacity = 1,
            };
            DIR *item_dir = opendir("wz/Item.wz");

            ITEM_INFOS = malloc(sizeof(struct ItemInfo));

            struct dirent *entry;
            while ((entry = readdir(item_dir)) != NULL) {
                // Skip Pet and Special for now
                if (!strcmp(entry->d_name, "Pet"))
                    continue;
                if (!strcmp(entry->d_name, "Special"))
                    continue;
                if (entry->d_name[0] != '.' && entry->d_type == DT_DIR) {
                    DIR *dir = fdopendir(openat(dirfd(item_dir), entry->d_name, O_RDONLY));
                    while ((entry = readdir(dir)) != NULL) {
                        if (entry->d_name[0] == '.' || entry->d_type != DT_REG)
                            continue;
                        int fd = openat(dirfd(dir), entry->d_name, O_RDONLY);
                        off_t len = lseek(fd, 0, SEEK_END);
                        lseek(fd, 0, SEEK_SET);
                        char *data = malloc(len);
                        read(fd, data, len);
                        close(fd);

                        XML_SetElementHandler(parser, on_item_start, on_item_end);
                        XML_SetUserData(parser, &ctx);
                        XML_Parse(parser, data, len, true);
                        free(data);
                        XML_ParserReset(parser, NULL);
                    }
                    closedir(dir);
                }
            }
            closedir(item_dir);

            ctx.head2 = NULL;
            ctx.skip = 0;

            DIR *equip_dir = opendir("wz/Character.wz");

            while ((entry = readdir(equip_dir)) != NULL) {
                if (entry->d_type == DT_DIR && entry->d_name[0] != '.' && strcmp(entry->d_name, "Afterimage")) {
                    int fd = openat(dirfd(equip_dir), entry->d_name, O_RDONLY | O_DIRECTORY);
                    DIR *dir = fdopendir(fd);
                    while ((entry = readdir(dir)) != NULL) {
                        if (entry->d_name[0] == '.')
                            continue;
                        fd = openat(dirfd(dir), entry->d_name, O_RDONLY);
                        off_t len = lseek(fd, 0, SEEK_END);
                        lseek(fd, 0, SEEK_SET);
                        char *data = malloc(len);
                        read(fd, data, len);
                        close(fd);

                        XML_SetElementHandler(parser, on_equip_item_start, on_equip_item_end);
                        XML_SetUserData(parser, &ctx);
                        XML_Parse(parser, data, len, true);
                        free(data);
                        XML_ParserReset(parser, NULL);
                    }
                    closedir(dir);
                }
            }
            closedir(equip_dir);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(ITEM_INFOS, sizeof(struct ItemInfo), offsetof(struct ItemInfo, id), sizeof(uint32_t), ITEM_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            ITEM_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < ITEM_INFO_COUNT) {
                uint32_t j = cmph_search(ITEM_INFO_MPH, (void *)&ITEM_INFOS[i].id, sizeof(uint32_t));
                if (i != j) {
                    struct ItemInfo temp = ITEM_INFOS[j];
                    ITEM_INFOS[j] = ITEM_INFOS[i];
                    ITEM_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            fprintf(stderr, "Loaded items\n");
        }

        {
            struct ConsumableParserContext ctx = {
                .head = NULL,
                .itemCapacity = 1,
            };
            DIR *dir = opendir("wz/Item.wz/Consume");

            CONSUMABLE_INFOS = malloc(sizeof(struct ConsumableInfo));

            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.' || entry->d_type != DT_REG)
                    continue;
                int fd = openat(dirfd(dir), entry->d_name, O_RDONLY);
                off_t len = lseek(fd, 0, SEEK_END);
                lseek(fd, 0, SEEK_SET);
                char *data = malloc(len);
                read(fd, data, len);
                close(fd);

                XML_SetElementHandler(parser, on_consumable_start, on_consumable_end);
                XML_SetUserData(parser, &ctx);
                XML_Parse(parser, data, len, true);
                free(data);
                XML_ParserReset(parser, NULL);
            }
            closedir(dir);

            cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(CONSUMABLE_INFOS, sizeof(struct ConsumableInfo), offsetof(struct ConsumableInfo, id), sizeof(uint32_t), CONSUMABLE_INFO_COUNT);
            cmph_config_t *config = cmph_config_new(adapter);
            cmph_config_set_algo(config, CMPH_BDZ);
            CONSUMABLE_INFO_MPH = cmph_new(config);
            cmph_config_destroy(config);
            cmph_io_struct_vector_adapter_destroy(adapter);
            size_t i = 0;
            while (i < CONSUMABLE_INFO_COUNT) {
                uint32_t j = cmph_search(CONSUMABLE_INFO_MPH, (void *)&CONSUMABLE_INFOS[i].id, sizeof(uint32_t));
                if (i != j) {
                    struct ConsumableInfo temp = CONSUMABLE_INFOS[j];
                    CONSUMABLE_INFOS[j] = CONSUMABLE_INFOS[i];
                    CONSUMABLE_INFOS[i] = temp;
                } else {
                    i++;
                }
            }

            fprintf(stderr, "Loaded consumables\n");
        }

        XML_ParserFree(parser);
        //sem_post(sem);
    }

    //sem_close(sem);

    return 0;
}

int wz_init_equipment(void)
{
    XML_Parser parser = XML_ParserCreate(NULL);

    {
        struct EquipParserContext ctx = {
            .parser = parser,
            .head = NULL,
        };
        DIR *equip_dir = opendir("wz/Character.wz");
        struct dirent *entry;
        while ((entry = readdir(equip_dir)) != NULL) {
            if (entry->d_type == DT_DIR && entry->d_name[0] != '.' && strcmp(entry->d_name, "Afterimage")) {
                int fd = openat(dirfd(equip_dir), entry->d_name, O_RDONLY | O_DIRECTORY);
                DIR *dir = fdopendir(fd);
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_name[0] != '.')
                        EQUIP_INFO_COUNT++;
                }
                closedir(dir);
            }
        }

        EQUIP_INFOS = malloc(EQUIP_INFO_COUNT * sizeof(struct EquipInfo));
        rewinddir(equip_dir);

        while ((entry = readdir(equip_dir)) != NULL) {
            if (entry->d_type == DT_DIR && entry->d_name[0] != '.' && strcmp(entry->d_name, "Afterimage")) {
                int fd = openat(dirfd(equip_dir), entry->d_name, O_RDONLY | O_DIRECTORY);
                DIR *dir = fdopendir(fd);
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_name[0] == '.')
                        continue;
                    fd = openat(dirfd(dir), entry->d_name, O_RDONLY);
                    off_t len = lseek(fd, 0, SEEK_END);
                    lseek(fd, 0, SEEK_SET);
                    char *data = malloc(len);
                    read(fd, data, len);
                    close(fd);

                    XML_SetElementHandler(parser, on_equip_start, on_equip_end);
                    XML_SetUserData(parser, &ctx);
                    XML_Parse(parser, data, len, true);
                    free(data);
                    XML_ParserReset(parser, NULL);
                    ctx.currentEquip++;
                }
                closedir(dir);
            }
        }
        closedir(equip_dir);

        cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(EQUIP_INFOS, sizeof(struct EquipInfo), offsetof(struct EquipInfo, id), sizeof(uint32_t), EQUIP_INFO_COUNT);
        cmph_config_t *config = cmph_config_new(adapter);
        cmph_config_set_algo(config, CMPH_BDZ);
        EQUIP_INFO_MPH = cmph_new(config);
        assert(EQUIP_INFO_MPH != NULL);
        cmph_config_destroy(config);
        cmph_io_struct_vector_adapter_destroy(adapter);
        size_t i = 0;
        while (i < EQUIP_INFO_COUNT) {
            uint32_t j = cmph_search(EQUIP_INFO_MPH, (void *)&EQUIP_INFOS[i].id, sizeof(uint32_t));
            if (i != j) {
                struct EquipInfo temp = EQUIP_INFOS[j];
                EQUIP_INFOS[j] = EQUIP_INFOS[i];
                EQUIP_INFOS[i] = temp;
            } else {
                i++;
            }
        }

        fprintf(stderr, "Loaded equipment\n");
    }

    XML_ParserFree(parser);

    return 0;
}

static void foothold_tree_free(struct RTreeNode *node)
{
    if (node == NULL)
        return;

    if (node->isLeaf) {
        free(node);
        return;
    }

    for (uint8_t i = 0; i < node->count; i++)
        foothold_tree_free(node->children[i]);

    free(node);
}

void wz_terminate(void)
{
    cmph_destroy(REACTOR_INFO_MPH);
    for (size_t i = 0; i < REACTOR_INFO_COUNT; i++) {
        struct ReactorInfo *reactor = &REACTOR_INFOS[i];
        for (size_t i = 0; i < reactor->stateCount; i++) {
            for (size_t j = 0; j < reactor->states[i].eventCount; j++) {
                if (reactor->states[i].events[j].type == REACTOR_EVENT_TYPE_SKILL)
                    free(reactor->states[i].events[j].skills);
            }
            free(reactor->states[i].events);
        }
        free(reactor->states);
    }
    free(REACTOR_INFOS);

    cmph_destroy(CONSUMABLE_INFO_MPH);
    free(CONSUMABLE_INFOS);
    cmph_destroy(EQUIP_INFO_MPH);
    free(EQUIP_INFOS);
    cmph_destroy(ITEM_INFO_MPH);
    free(ITEM_INFOS);
    cmph_destroy(QUEST_INFO_MPH);
    for (size_t i = 0; i < QUEST_INFO_COUNT; i++) {
        for (size_t j = 0; j < QUEST_INFOS[i].endActCount; j++) {
            if (QUEST_INFOS[i].endActs[j].type == QUEST_ACT_TYPE_ITEM)
                free(QUEST_INFOS[i].endActs[j].item.items);
        }
        free(QUEST_INFOS[i].endActs);
        for (size_t j = 0; j < QUEST_INFOS[i].startActCount; j++) {
            if (QUEST_INFOS[i].startActs[j].type == QUEST_ACT_TYPE_ITEM)
                free(QUEST_INFOS[i].startActs[j].item.items);
        }
        free(QUEST_INFOS[i].startActs);
        for (size_t j = 0; j < QUEST_INFOS[i].endRequirementCount; j++) {
            if (QUEST_INFOS[i].endRequirements[j].type == QUEST_REQUIREMENT_TYPE_JOB)
                free(QUEST_INFOS[i].endRequirements[j].job.jobs);
            else if (QUEST_INFOS[i].endRequirements[j].type == QUEST_REQUIREMENT_TYPE_MOB)
                free(QUEST_INFOS[i].endRequirements[j].mob.mobs);
            else if (QUEST_INFOS[i].endRequirements[j].type == QUEST_REQUIREMENT_TYPE_INFO) {
                for (size_t k = 0; k < QUEST_INFOS[i].endRequirements[j].info.infoCount; k++)
                    free(QUEST_INFOS[i].endRequirements[j].info.infos[k]);

                free(QUEST_INFOS[i].endRequirements[j].info.infos);
            }
        }
        free(QUEST_INFOS[i].endRequirements);
        for (size_t j = 0; j < QUEST_INFOS[i].startRequirementCount; j++) {
            if (QUEST_INFOS[i].startRequirements[j].type == QUEST_REQUIREMENT_TYPE_JOB)
                free(QUEST_INFOS[i].startRequirements[j].job.jobs);
            else if (QUEST_INFOS[i].startRequirements[j].type == QUEST_REQUIREMENT_TYPE_INFO) {
                for (size_t k = 0; k < QUEST_INFOS[i].startRequirements[j].info.infoCount; k++)
                    free(QUEST_INFOS[i].startRequirements[j].info.infos[k]);

                free(QUEST_INFOS[i].startRequirements[j].info.infos);
            }
        }
        free(QUEST_INFOS[i].startRequirements);
    }
    free(QUEST_INFOS);

    cmph_destroy(MOB_INFO_MPH);
    free(MOB_INFOS);

    cmph_destroy(MAP_INFO_MPH);

    for (size_t i = 0; i < MAP_INFO_COUNT; i++) {
        foothold_tree_free(MAP_INFOS[i].footholdTree->root);
        free(MAP_INFOS[i].footholdTree);
        free(MAP_INFOS[i].lives);
        free(MAP_INFOS[i].portals);
        free(MAP_INFOS[i].reactors);
    }
    free(MAP_INFOS);
    //if (WRITER) {
    //    sem_unlink("/syrup-wz");
    //    shm_unlink("/syrup-wz");
    //}
}

void wz_terminate_equipment(void)
{
    cmph_destroy(EQUIP_INFO_MPH);
    free(EQUIP_INFOS);
}

uint32_t wz_get_map_nearest_town(uint32_t id)
{
    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    return MAP_INFOS[i].returnMap;
}

uint32_t wz_get_target_map(uint32_t id, char *target)
{
    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    for (size_t j = 0; j < MAP_INFOS[i].portalCount; j++) {
        if (!strcmp(MAP_INFOS[i].portals[j].name, target)) {
            return MAP_INFOS[i].portals[j].targetMap;
        }
    }

    return -1;
}

uint8_t wz_get_target_portal(uint32_t id, char *target)
{
    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    uint32_t target_map_id;
    char *name;
    for (size_t j = 0; j < MAP_INFOS[i].portalCount; j++) {
        if (!strcmp(MAP_INFOS[i].portals[j].name, target)) {
            target_map_id = MAP_INFOS[i].portals[j].targetMap;
            name = MAP_INFOS[i].portals[j].targetName;
        }
    }

    i = cmph_search(MAP_INFO_MPH, (void *)&target_map_id, sizeof(uint32_t));
    for (size_t j = 0; j < MAP_INFOS[i].portalCount; j++) {
        if (!strcmp(MAP_INFOS[i].portals[j].name, name)) {
            return j;
        }
    }

    return -1;
}

static int16_t get_distance_below(struct Foothold *fh, struct Point *p)
{
    if (fh->p1.x == fh->p2.x) {
        return MIN(fh->p1.y, fh->p2.y) - p->y;
    }

    return (fh->p2.y - fh->p1.y) * (p->x - fh->p1.x) / (fh->p2.x - fh->p1.x) + fh->p1.y - p->y;
}

static struct Foothold *tree_find_below(struct RTreeNode *node, struct Point *p)
{
    if (node->bound.sw.x > p->x || node->bound.sw.y < p->y)
        return NULL;

    if (node->isLeaf) {
        struct Foothold *min = NULL;
        int16_t min_dist = INT16_MAX;
        for (uint8_t i = 0; i < node->count; i++) {
            if ((node->footholds[i].p1.x > p->x && node->footholds[i].p2.x > p->x) || (node->footholds[i].p1.x < p->x && node->footholds[i].p2.x < p->x) || (node->footholds[i].p1.y < p->y && node->footholds[i].p2.y < p->y))
                continue;

            if (node->footholds[i].p1.x == node->footholds[i].p2.x && (p->x != node->footholds[i].p1.x || p->y > node->footholds[i].p1.y || p->y > node->footholds[i].p2.y))
                continue;

            int16_t dist = get_distance_below(&node->footholds[i], p);
            if (dist < 0)
                continue;

            if (dist < min_dist) {
                min = &node->footholds[i];
                min_dist = dist;
            }
        }

        return min;
    }

    struct Foothold *min = NULL;
    int16_t min_dist = INT16_MAX;
    for (uint8_t i = 0; i < node->count; i++) {
        struct Foothold *current = tree_find_below(node->children[i], p);
        if (current != NULL) {
            int16_t dist = get_distance_below(current, p);
            if (dist < min_dist) {
                min = current;
                min_dist = dist;
            }
        }
    }

    return min;
}

const struct Foothold *foothold_tree_find_below(const struct FootholdRTree *tree, struct Point *p)
{
    return tree_find_below(tree->root, p);
}

const struct FootholdRTree *wz_get_foothold_tree_for_map(uint32_t id)
{
    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    return MAP_INFOS[i].footholdTree;
}

const struct LifeInfo *wz_get_life_for_map(uint32_t id, size_t *count)
{
    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    *count = MAP_INFOS[i].lifeCount;
    return MAP_INFOS[i].lives;
}

const struct MapReactorInfo *wz_get_reactors_for_map(uint32_t id, size_t *count)
{
    size_t count_;
    if (count == NULL)
        count = &count_;

    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    *count = MAP_INFOS[i].reactorCount;
    return MAP_INFOS[i].reactors;
}

const struct PortalInfo *wz_get_portal_info_by_name(uint32_t id, const char *name)
{
    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    if (MAP_INFOS[i].id != id)
        return NULL;

    for (size_t j = 0; j < MAP_INFOS[i].portalCount; j++) {
        if (!strcmp(MAP_INFOS[i].portals[j].name, name))
            return &MAP_INFOS[i].portals[j];
    }

    return NULL;
}

const struct PortalInfo *wz_get_portal_info(uint32_t id, uint8_t pid)
{
    size_t i = cmph_search(MAP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    if (MAP_INFOS[i].id != id)
        return NULL;

    if (pid >= MAP_INFOS[i].portalCount)
        return NULL;

    return &MAP_INFOS[i].portals[pid];
}

const struct EquipInfo *wz_get_equip_info(uint32_t id)
{
    struct EquipInfo *info = &EQUIP_INFOS[cmph_search(EQUIP_INFO_MPH, (void *)&id, sizeof(uint32_t))];
    if (info->id != id)
        return NULL;
    return info;
}

const struct ConsumableInfo *wz_get_consumable_info(uint32_t id)
{
    struct ConsumableInfo *info = &CONSUMABLE_INFOS[cmph_search(CONSUMABLE_INFO_MPH, (void *)&id, sizeof(uint32_t))];
    if (info->id != id)
        return NULL;
    return info;
}

const struct MobInfo *wz_get_monster_stats(uint32_t id)
{
    struct MobInfo *info = &MOB_INFOS[cmph_search(MOB_INFO_MPH, (void *)&id, sizeof(uint32_t))];
    if (info->id != id)
        return NULL;
    return info;
}

const struct QuestInfo *wz_get_quest_info(uint16_t id)
{
    struct QuestInfo *info = &QUEST_INFOS[cmph_search(QUEST_INFO_MPH, (void *)&id, sizeof(uint16_t))];
    if (info->id != id)
        return NULL;
    return info;
}

const struct ItemInfo *wz_get_item_info(uint32_t id)
{
    struct ItemInfo *info = &ITEM_INFOS[cmph_search(ITEM_INFO_MPH, (void *)&id, sizeof(uint32_t))];
    if (info->id != id)
        return NULL;
    return info;

}

const struct ReactorInfo *wz_get_reactor_info(uint32_t id)
{
    struct ReactorInfo *info = &REACTOR_INFOS[cmph_search(REACTOR_INFO_MPH, (void *)&id, sizeof(uint32_t))];
    if (info->id != id)
        return NULL;
    return info;
}

static void on_map_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct MapParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        ctx->head = malloc(sizeof(struct MapParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = MAP_ITEM_TYPE_TOP_LEVEL;
        assert(!strcmp(name, "imgdir"));

        const char *id = NULL;
        for (size_t i = 0; attrs[i] != NULL; i += 2) {
            if (!strcmp(attrs[i], "name"))
                id = attrs[i+1];
        }

        assert(id != NULL);

        MAP_INFOS[ctx->currentMap].id = strtol(id, NULL, 10);
    } else {
        switch (ctx->head->type) {
        case MAP_ITEM_TYPE_TOP_LEVEL:
            assert(!strcmp(name, "imgdir"));

            size_t i = 0;
            while (attrs[i] != NULL && strcmp(attrs[i], "name"))
                i += 2;

            assert(attrs[i] != NULL);

            i++;
            if (!strcmp(attrs[i], "info")) {
                struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
                new->next = ctx->head;
                new->type = MAP_ITEM_TYPE_INFO;
                ctx->head = new;
            } else if (!strcmp(attrs[i], "life")) {
                struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
                new->next = ctx->head;
                new->type = MAP_ITEM_TYPE_LIVES;
                ctx->head = new;
            } else if (!strcmp(attrs[i], "reactor")) {
                struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
                new->next = ctx->head;
                new->type = MAP_ITEM_TYPE_REACTORS;
                ctx->head = new;
            } else if (!strcmp(attrs[i], "portal")) {
                struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
                new->next = ctx->head;
                new->type = MAP_ITEM_TYPE_PORTALS;
                ctx->head = new;
            } else if (!strcmp(attrs[i], "foothold")) {
                struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
                new->next = ctx->head;
                new->type = MAP_ITEM_TYPE_FOOTHOLDS;
                ctx->head = new;
            } else {
                ctx->skip++;
            }
        break;

        case MAP_ITEM_TYPE_INFO: {
            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            ctx->skip++;
            if (!strcmp(name, "int")) {
                if (!strcmp(key, "returnMap")) {
                    MAP_INFOS[ctx->currentMap].returnMap = strtol(value, NULL, 10);
                } else if (!strcmp(key, "fieldLimit")) {
                } else if (!strcmp(key, "VRTop")) {
                } else if (!strcmp(key, "VRBottom")) {
                } else if (!strcmp(key, "VRLeft")) {
                } else if (!strcmp(key, "VRRight")) {
                }
            } else if (!strcmp(name, "float")) {
                if (!strcmp(key, "mobRate")) {
                    MAP_INFOS[ctx->currentMap].mobRate = strtof(value, NULL);
                }
            } else if (!strcmp(name, "string")) {
                if (!strcmp(key, "onFirstUserEnter")) {
                } else if (!strcmp(key, "onUserEnter")) {
                }
            }
        }
        break;

        case MAP_ITEM_TYPE_FOOTHOLDS: {
            if (ctx->footholdLevel == 2) {
                ctx->currentFoothold.id = strtol(attrs[1], NULL, 10);
                ctx->footholdLevel++;
            } else if (ctx->footholdLevel == 3) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                ctx->skip++;
                if (!strcmp(key, "x1")) {
                    ctx->currentFoothold.p1.x = strtol(value, NULL, 10);
                } else if (!strcmp(key, "y1")) {
                    ctx->currentFoothold.p1.y = strtol(value, NULL, 10);
                } else if (!strcmp(key, "x2")) {
                    ctx->currentFoothold.p2.x = strtol(value, NULL, 10);
                } else if (!strcmp(key, "y2")) {
                    ctx->currentFoothold.p2.y = strtol(value, NULL, 10);
                }
            } else {
                ctx->footholdLevel++;
            }
        }
        break;

        case MAP_ITEM_TYPE_LIVES: {
            if (MAP_INFOS[ctx->currentMap].lifeCount == ctx->currentLife) {
                MAP_INFOS[ctx->currentMap].lives = realloc(MAP_INFOS[ctx->currentMap].lives, (ctx->currentLife * 2) * sizeof(struct LifeInfo));
                ctx->currentLife *= 2;
            }

            MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].f = false;

            struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
            new->next = ctx->head;
            new->type = MAP_ITEM_TYPE_LIFE;
            ctx->head = new;
        }
        break;

        case MAP_ITEM_TYPE_LIFE: {
            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            ctx->skip++;
            if (!strcmp(name, "int")) {
                if (!strcmp(key, "x")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].spawnPoint.x = strtol(value, NULL, 10);
                } else if (!strcmp(key, "y")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].spawnPoint.y = strtol(value, NULL, 10);
                } else if (!strcmp(key, "fh")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].fh = strtol(value, NULL, 10);
                } else if (!strcmp(key, "cy")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].cy = strtol(value, NULL, 10);
                } else if (!strcmp(key, "rx0")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].rx0 = strtol(value, NULL, 10);
                } else if (!strcmp(key, "rx1")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].rx1 = strtol(value, NULL, 10);
                } else if (!strcmp(key, "f")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].f = strtol(value, NULL, 10);
                }
            } else if (!strcmp(name, "string")) {
                if (!strcmp(key, "type")) {
                    if (!strcmp(value, "n")) {
                        MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].type = LIFE_TYPE_NPC;
                    } else if (!strcmp(value, "m")) {
                        MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].type = LIFE_TYPE_MOB;
                    } else {
                        MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].type = LIFE_TYPE_UNKNOWN;
                    }
                } else if (!strcmp(key, "id")) {
                    MAP_INFOS[ctx->currentMap].lives[MAP_INFOS[ctx->currentMap].lifeCount].id = strtol(value, NULL, 10);
                }
            } else {
                assert(0); // ERROR
            }
        }
        break;

        case MAP_ITEM_TYPE_PORTALS: {
            if (ctx->currentPortal == MAP_INFOS[ctx->currentMap].portalCount) {
                MAP_INFOS[ctx->currentMap].portals = realloc(MAP_INFOS[ctx->currentMap].portals, (ctx->currentPortal * 2) * sizeof(struct PortalInfo));
                ctx->currentPortal *= 2;
            }

            MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].id = MAP_INFOS[ctx->currentMap].portalCount;

            struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
            new->next = ctx->head;
            new->type = MAP_ITEM_TYPE_PORTAL;
            ctx->head = new;
        }
        break;

        case MAP_ITEM_TYPE_PORTAL: {
            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            ctx->skip++;
            if (!strcmp(name, "int")) {
                if (!strcmp(key, "pt")) {
                    MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].type = PORTAL_TYPE_REGULAR;
                } else if (!strcmp(key, "x")) {
                    MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].x = strtol(value, NULL, 10);
                } else if (!strcmp(key, "y")) {
                    MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].y = strtol(value, NULL, 10);
                } else if (!strcmp(key, "tm")) {
                    MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].targetMap = strtol(value, NULL, 10);
                } else {
                    //assert(0);
                }
            } else if (!strcmp(name, "string")) {
                if (!strcmp(key, "pn")) {
                    strcpy(MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].name, value);
                } else if (!strcmp(key, "tn")) {
                    strcpy(MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].targetName, value);
                } else if (!strcmp(key, "script")) {
                    strcpy(MAP_INFOS[ctx->currentMap].portals[MAP_INFOS[ctx->currentMap].portalCount].script, value);
                }
            } else {
                assert(0);
            }
        }
        break;

        case MAP_ITEM_TYPE_REACTORS: {
            if (ctx->reactorCapacity == MAP_INFOS[ctx->currentMap].reactorCount) {
                MAP_INFOS[ctx->currentMap].reactors = realloc(MAP_INFOS[ctx->currentMap].reactors, (ctx->reactorCapacity * 2) * sizeof(struct MapReactorInfo));
                ctx->reactorCapacity *= 2;
            }
            MAP_INFOS[ctx->currentMap].reactors[MAP_INFOS[ctx->currentMap].reactorCount].id = MAP_INFOS[ctx->currentMap].reactorCount;

            struct MapParserStackNode *new = malloc(sizeof(struct MapParserStackNode));
            new->next = ctx->head;
            new->type = MAP_ITEM_TYPE_REACTOR;
            ctx->head = new;
        }
        break;

        case MAP_ITEM_TYPE_REACTOR: {
            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            ctx->skip++;
            if (!strcmp(name, "int")) {
                if (!strcmp(key, "x")) {
                    MAP_INFOS[ctx->currentMap].reactors[MAP_INFOS[ctx->currentMap].reactorCount].pos.x = strtol(value, NULL, 10);
                } else if (!strcmp(key, "y")) {
                    MAP_INFOS[ctx->currentMap].reactors[MAP_INFOS[ctx->currentMap].reactorCount].pos.y = strtol(value, NULL, 10);
                } else if (!strcmp(key, "reactorTime")) {
                    MAP_INFOS[ctx->currentMap].reactors[MAP_INFOS[ctx->currentMap].reactorCount].reactorTime = strtol(value, NULL, 10) == 1;
                } else if (!strcmp(key, "f")) {
                    MAP_INFOS[ctx->currentMap].reactors[MAP_INFOS[ctx->currentMap].reactorCount].f = strtol(value, NULL, 10) == 1;
                } else {
                    //assert(0);
                }
            } else if (!strcmp(name, "string")) {
                if (!strcmp(key, "id")) {
                    MAP_INFOS[ctx->currentMap].reactors[MAP_INFOS[ctx->currentMap].reactorCount].id = strtol(value, NULL, 10);
                }
                // else if (!strcmp(key, "name")) {
                //     strcpy(MAP_INFOS[ctx->currentMap].reactors[MAP_INFOS[ctx->currentMap].reactorCount - 1].name, value);
                // }
            } else {
                assert(0);
            }
        }
        break;

        default:
            ctx->skip++;
        }
    }
}

static void on_map_end(void *user_data, const XML_Char *name)
{
    struct MapParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->footholdLevel > 0) {
        if (ctx->footholdLevel == 3)
            insert_foothold(MAP_INFOS[ctx->currentMap].footholdTree, &ctx->currentFoothold);

        ctx->footholdLevel--;
        return;
    } else if (ctx->head->type == MAP_ITEM_TYPE_LIFE) {
        MAP_INFOS[ctx->currentMap].lifeCount++;
    } else if (ctx->head->type == MAP_ITEM_TYPE_LIVES) {
        if (MAP_INFOS[ctx->currentMap].lifeCount == 0) {
            free(MAP_INFOS[ctx->currentMap].lives);
            MAP_INFOS[ctx->currentMap].lives = NULL;
        }
    } else if (ctx->head->type == MAP_ITEM_TYPE_PORTAL) {
        MAP_INFOS[ctx->currentMap].portalCount++;
    } else if (ctx->head->type == MAP_ITEM_TYPE_PORTALS) {
        if (MAP_INFOS[ctx->currentMap].portalCount == 0) {
            free(MAP_INFOS[ctx->currentMap].portals);
            MAP_INFOS[ctx->currentMap].portals = NULL;
        }
    } else if (ctx->head->type == MAP_ITEM_TYPE_REACTOR) {
        MAP_INFOS[ctx->currentMap].reactorCount++;
    } else if (ctx->head->type == MAP_ITEM_TYPE_REACTORS) {
        if (MAP_INFOS[ctx->currentMap].reactorCount == 0) {
            free(MAP_INFOS[ctx->currentMap].reactors);
            MAP_INFOS[ctx->currentMap].reactors = NULL;
        }
    }

    struct MapParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_mob_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct MobParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        ctx->head = malloc(sizeof(struct MobParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = MOB_ITEM_TYPE_TOP_LEVEL;
        assert(!strcmp(name, "imgdir"));

        const char *id = NULL;
        for (size_t i = 0; attrs[i] != NULL; i += 2) {
            if (!strcmp(attrs[i], "name"))
                id = attrs[i+1];
        }

        assert(id != NULL);

        MOB_INFOS[MOB_INFO_COUNT].id = strtol(id, NULL, 10);
        MOB_INFOS[MOB_INFO_COUNT].bodyAttack = false;
        MOB_INFOS[MOB_INFO_COUNT].exp = 0;
        MOB_INFOS[MOB_INFO_COUNT].undead = false;
        MOB_INFOS[MOB_INFO_COUNT].boss = false;
    } else {
        switch (ctx->head->type) {
        case MOB_ITEM_TYPE_TOP_LEVEL:
            assert(!strcmp(name, "imgdir"));

            size_t i = 0;
            while (attrs[i] != NULL && strcmp(attrs[i], "name"))
                i += 2;

            assert(attrs[i] != NULL);

            i++;
            if (!strcmp(attrs[i], "info")) {
                struct MobParserStackNode *new = malloc(sizeof(struct MobParserStackNode));
                new->next = ctx->head;
                new->type = MOB_ITEM_TYPE_INFO;
                ctx->head = new;
            } else {
                ctx->skip++;
            }
        break;

        case MOB_ITEM_TYPE_INFO: {
            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            ctx->skip++;
            if (!strcmp(name, "int")) {
                if (!strcmp(key, "bodyAttack")) {
                    MOB_INFOS[MOB_INFO_COUNT].bodyAttack = strtol(value, NULL, 10);
                } else if (!strcmp(key, "level")) {
                    MOB_INFOS[MOB_INFO_COUNT].level = strtol(value, NULL, 10);
                } else if (!strcmp(key, "maxHP")) {
                    MOB_INFOS[MOB_INFO_COUNT].hp = strtol(value, NULL, 10);
                } else if (!strcmp(key, "maxMP")) {
                    MOB_INFOS[MOB_INFO_COUNT].mp = strtol(value, NULL, 10);
                } else if (!strcmp(key, "speed")) {
                    MOB_INFOS[MOB_INFO_COUNT].speed = strtol(value, NULL, 10);
                } else if (!strcmp(key, "flySpeed")) {
                    MOB_INFOS[MOB_INFO_COUNT].speed = strtol(value, NULL, 10);
                } else if (!strcmp(key, "PADamage")) {
                    MOB_INFOS[MOB_INFO_COUNT].atk = strtol(value, NULL, 10);
                } else if (!strcmp(key, "PDDamage")) {
                    MOB_INFOS[MOB_INFO_COUNT].def = strtol(value, NULL, 10);
                } else if (!strcmp(key, "MADamage")) {
                    MOB_INFOS[MOB_INFO_COUNT].matk = strtol(value, NULL, 10);
                } else if (!strcmp(key, "MDDamage")) {
                    MOB_INFOS[MOB_INFO_COUNT].mdef = strtol(value, NULL, 10);
                } else if (!strcmp(key, "acc")) {
                    MOB_INFOS[MOB_INFO_COUNT].acc = strtol(value, NULL, 10);
                } else if (!strcmp(key, "eva")) {
                    MOB_INFOS[MOB_INFO_COUNT].avoid = strtol(value, NULL, 10);
                } else if (!strcmp(key, "exp")) {
                    MOB_INFOS[MOB_INFO_COUNT].exp = strtol(value, NULL, 10);
                } else if (!strcmp(key, "undead")) {
                    MOB_INFOS[MOB_INFO_COUNT].undead = strtol(value, NULL, 10);
                } else if (!strcmp(key, "boss")) {
                    MOB_INFOS[MOB_INFO_COUNT].boss = strtol(value, NULL, 10);
                }
            }
        }
        break;

        default:
            ctx->skip++;
        }
    }
}

static void on_mob_end(void *user_data, const XML_Char *name)
{
    struct MobParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == MOB_ITEM_TYPE_INFO) {
        // We don't need any further information except for the mob's info
        free(ctx->head->next);
        free(ctx->head);
        ctx->head = NULL;
        XML_StopParser(ctx->parser, false);
        return;
    }
    struct MobParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_quest_check_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct QuestCheckParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        ctx->head = malloc(sizeof(struct QuestCheckParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = QUEST_CHECK_ITEM_TYPE_TOP_LEVEL;
        assert(!strcmp(name, "imgdir"));
    } else {
        switch (ctx->head->type) {
        case QUEST_CHECK_ITEM_TYPE_TOP_LEVEL:
            assert(!strcmp(name, "imgdir"));

            if (ctx->questCapacity == QUEST_INFO_COUNT) {
                QUEST_INFOS = realloc(QUEST_INFOS, (ctx->questCapacity * 2) * sizeof(struct QuestInfo));
                ctx->questCapacity *= 2;
            }

            uint16_t id = strtol(attrs[1], NULL, 10);
            QUEST_INFOS[QUEST_INFO_COUNT].id = id;
            QUEST_INFOS[QUEST_INFO_COUNT].startScript = false;
            QUEST_INFOS[QUEST_INFO_COUNT].endScript = false;
            QUEST_INFOS[QUEST_INFO_COUNT].startActCount = 0;
            QUEST_INFOS[QUEST_INFO_COUNT].startActs = NULL;
            QUEST_INFOS[QUEST_INFO_COUNT].endActCount = 0;
            QUEST_INFOS[QUEST_INFO_COUNT].endActs = NULL;
            struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_CHECK_ITEM_TYPE_QUEST;
            ctx->head = new;
        break;

        case QUEST_CHECK_ITEM_TYPE_QUEST: {
            struct QuestInfo *quest = &QUEST_INFOS[QUEST_INFO_COUNT];
            assert(!strcmp(name, "imgdir"));
            assert(!strcmp(attrs[0], "name"));

            struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
            new->next = ctx->head;
            ctx->head = new;
            long t = strtol(attrs[1], NULL, 10);
            if (t == 0) {
                quest->startRequirements = malloc(sizeof(struct QuestRequirement));
                quest->startRequirementCount = 0;
                ctx->reqCapacity = 1;
                new->type = QUEST_CHECK_ITEM_TYPE_START;
            } else if (t == 1) {
                quest->endRequirements = malloc(sizeof(struct QuestRequirement));
                quest->endRequirementCount = 0;
                ctx->reqCapacity = 1;
                new->type = QUEST_CHECK_ITEM_TYPE_END;
            } else {
                fprintf(stderr, "Warning: Not a start or end requiremnt imgdir in quest %hu\n", quest->id);
                // In quest 4940 there is quest 4961, probably there by accident
                // as it should be in its own imgdir, it seems like this quest is unused
                // and was probably superseded by quest 4955 - 'Psycho Jack'
                ctx->skip++;
                ctx->head = new->next;
                free(new);
            }
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_START: {
            struct QuestInfo *quest = &QUEST_INFOS[QUEST_INFO_COUNT];
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (quest->startRequirementCount == ctx->reqCapacity) {
                    quest->startRequirements = realloc(quest->startRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                    ctx->reqCapacity *= 2;
                }
                struct QuestRequirement *req = &quest->startRequirements[quest->startRequirementCount];
                if (!strcmp(key, "npc")) {
                    req->type = QUEST_REQUIREMENT_TYPE_NPC;
                    req->npc.id = strtol(value, NULL, 10);
                    quest->startRequirementCount++;
                } else if (!strcmp(key, "lvmin")) {
                    req->type = QUEST_REQUIREMENT_TYPE_MIN_LEVEL;
                    req->minLevel.level = strtol(value, NULL, 10);
                    quest->startRequirementCount++;
                } else if (!strcmp(key, "lvmax")) {
                    req->type = QUEST_REQUIREMENT_TYPE_MAX_LEVEL;
                    req->maxLevel.level = strtol(value, NULL, 10);
                    quest->startRequirementCount++;
                } else if (!strcmp(key, "interval")) {
                    req->type = QUEST_REQUIREMENT_TYPE_INTERVAL;
                    req->interval.hours = strtol(value, NULL, 10);
                    quest->startRequirementCount++;
                } else if (!strcmp(key, "questComplete")) {
                    req->type = QUEST_REQUIREMENT_TYPE_COMPLETED_QUEST;
                    req->questCompleted.amount = strtol(value, NULL, 10);
                    quest->startRequirementCount++;
                } else if (!strcmp(key, "infoNumber")) {
                    struct QuestRequirement *req = NULL;
                    for (size_t i = 0; i < quest->startRequirementCount; i++) {
                        if (quest->startRequirements[i].type == QUEST_REQUIREMENT_TYPE_INFO) {
                            req = &quest->startRequirements[i];
                            break;
                        }
                    }

                    if (req == NULL) {
                        req = &quest->startRequirements[quest->startRequirementCount];
                        req->type = QUEST_REQUIREMENT_TYPE_INFO;
                        req->info.infoCount = 0;
                        req->info.infos = NULL;
                        quest->startRequirementCount++;
                    }

                    req->info.number = strtol(value, NULL, 10);
                }
                ctx->skip++;
            } else if (!strcmp(name, "string")) {
                const XML_Char *key = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name")) {
                        key = attrs[i+1];
                        break;
                    }
                }

                assert(key != NULL);

                if (!strcmp(key, "startscript")) {
                    quest->startScript = true;
                }
                ctx->skip++;
            } else if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "item")) {
                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_ITEMS;
                    ctx->head = new;
                } else if (!strcmp(attrs[1], "quest")) {
                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_QUESTS;
                    ctx->head = new;
                } else if (!strcmp(attrs[1], "job")) {
                    if (quest->startRequirementCount == ctx->reqCapacity) {
                        quest->startRequirements = realloc(quest->startRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                        ctx->reqCapacity *= 2;
                    }

                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_JOB;
                    ctx->head = new;
                    quest->startRequirements[quest->startRequirementCount].type = QUEST_REQUIREMENT_TYPE_JOB;
                    quest->startRequirements[quest->startRequirementCount].job.jobs = malloc(sizeof(uint16_t));
                    ctx->capacity = 1;
                    quest->startRequirements[quest->startRequirementCount].job.count = 0;
                } else if (!strcmp(attrs[1], "infoex")) {
                    if (quest->startRequirementCount == ctx->reqCapacity) {
                        quest->startRequirements = realloc(quest->startRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                        ctx->reqCapacity *= 2;
                    }

                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_INFOS;
                    ctx->head = new;

                    struct QuestRequirement *req = NULL;
                    for (size_t i = 0; i < quest->startRequirementCount; i++) {
                        if (quest->startRequirements[i].type == QUEST_REQUIREMENT_TYPE_INFO) {
                            req = &quest->startRequirements[i];
                            break;
                        }
                    }

                    if (req == NULL) {
                        req = &quest->startRequirements[quest->startRequirementCount];
                        req->type = QUEST_REQUIREMENT_TYPE_INFO;
                        req->info.number = 0;
                        quest->startRequirementCount++;
                    }

                    req->info.infos = malloc(sizeof(char *));
                    req->info.infoCount = 0;

                    ctx->infoReq = req;
                    ctx->capacity = 1;
                } else {
                    ctx->skip++;
                }
            }
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_END: {
            struct QuestInfo *quest = &QUEST_INFOS[QUEST_INFO_COUNT];
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (quest->endRequirementCount == ctx->reqCapacity) {
                    quest->endRequirements = realloc(quest->endRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                    ctx->reqCapacity *= 2;
                }
                struct QuestRequirement *req = &quest->endRequirements[quest->endRequirementCount];
                if (!strcmp(key, "npc")) {
                    req->type = QUEST_REQUIREMENT_TYPE_NPC;
                    req->npc.id = strtol(value, NULL, 10);
                    quest->endRequirementCount++;
                } else if (!strcmp(key, "lvmin")) {
                    req->type = QUEST_REQUIREMENT_TYPE_MIN_LEVEL;
                    req->minLevel.level = strtol(value, NULL, 10);
                    quest->endRequirementCount++;
                } else if (!strcmp(key, "lvmax")) {
                    req->type = QUEST_REQUIREMENT_TYPE_MAX_LEVEL;
                    req->maxLevel.level = strtol(value, NULL, 10);
                    quest->endRequirementCount++;
                } else if (!strcmp(key, "interval")) {
                    req->type = QUEST_REQUIREMENT_TYPE_INTERVAL;
                    req->interval.hours = strtol(value, NULL, 10);
                    quest->endRequirementCount++;
                } else if (!strcmp(key, "questComplete")) {
                    req->type = QUEST_REQUIREMENT_TYPE_COMPLETED_QUEST;
                    req->questCompleted.amount = strtol(value, NULL, 10);
                    quest->endRequirementCount++;
                } else if (!strcmp(key, "infoNumber")) {
                    struct QuestRequirement *req = NULL;
                    for (size_t i = 0; i < quest->endRequirementCount; i++) {
                        if (quest->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_INFO) {
                            req = &quest->endRequirements[i];
                            break;
                        }
                    }

                    if (req == NULL) {
                        req = &quest->endRequirements[quest->endRequirementCount];
                        req->type = QUEST_REQUIREMENT_TYPE_INFO;
                        req->info.infoCount = 0;
                        req->info.infos = NULL;
                        quest->endRequirementCount++;
                    }

                    req->info.number = strtol(value, NULL, 10);
                }
                ctx->skip++;
            } else if (!strcmp(name, "string")) {
                const XML_Char *key = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name")) {
                        key = attrs[i+1];
                        break;
                    }
                }

                assert(key != NULL);

                if (!strcmp(key, "endscript"))
                    quest->endScript = true;
                ctx->skip++;
            } else if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "item")) {
                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_ITEMS;
                    ctx->head = new;
                } else if (!strcmp(attrs[1], "quest")) {
                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_QUESTS;
                    ctx->head = new;
                } else if (!strcmp(attrs[1], "mob")) {
                    if (quest->endRequirementCount == ctx->reqCapacity) {
                        quest->endRequirements = realloc(quest->endRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                        ctx->reqCapacity *= 2;
                    }

                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_MOBS;
                    ctx->head = new;
                    quest->endRequirements[quest->endRequirementCount].type = QUEST_REQUIREMENT_TYPE_MOB;
                    quest->endRequirements[quest->endRequirementCount].mob.mobs = malloc(sizeof(*quest->endRequirements[quest->endRequirementCount].mob.mobs));
                    ctx->capacity = 1;
                    quest->endRequirements[quest->endRequirementCount].mob.count = 0;
                } else if (!strcmp(attrs[1], "job")) {
                    if (quest->endRequirementCount == ctx->reqCapacity) {
                        quest->endRequirements = realloc(QUEST_INFOS[QUEST_INFO_COUNT].endRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                        ctx->reqCapacity *= 2;
                    }

                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_JOB;
                    ctx->head = new;
                    quest->endRequirements[quest->endRequirementCount].type = QUEST_REQUIREMENT_TYPE_JOB;
                    quest->endRequirements[quest->endRequirementCount].job.jobs = malloc(sizeof(uint16_t));
                    ctx->capacity = 1;
                    quest->endRequirements[quest->endRequirementCount].job.count = 0;
                } else if (!strcmp(attrs[1], "infoex")) {
                    if (quest->endRequirementCount == ctx->reqCapacity) {
                        quest->endRequirements = realloc(quest->endRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                        ctx->reqCapacity *= 2;
                    }

                    struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_CHECK_ITEM_TYPE_REQ_INFOS;
                    ctx->head = new;

                    struct QuestRequirement *req = NULL;
                    for (size_t i = 0; i < quest->endRequirementCount; i++) {
                        if (quest->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_INFO) {
                            req = &quest->endRequirements[i];
                            break;
                        }
                    }

                    if (req == NULL) {
                        req = &quest->endRequirements[quest->endRequirementCount];
                        req->type = QUEST_REQUIREMENT_TYPE_INFO;
                        req->info.number = 0;
                        quest->endRequirementCount++;
                    }

                    req->info.infos = malloc(sizeof(char *));
                    req->info.infoCount = 0;

                    ctx->infoReq = req;
                    ctx->capacity = 1;
                } else {
                    ctx->skip++;
                }
            }
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_QUESTS: {
            if (strcmp(name, "imgdir"))
                assert(0);

            if (ctx->head->next->type == QUEST_CHECK_ITEM_TYPE_START)  {
                if (QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount == ctx->reqCapacity) {
                    QUEST_INFOS[QUEST_INFO_COUNT].startRequirements = realloc(QUEST_INFOS[QUEST_INFO_COUNT].startRequirements, ctx->reqCapacity * 2 * sizeof(struct QuestRequirement));
                    ctx->reqCapacity *= 2;
                }
                QUEST_INFOS[QUEST_INFO_COUNT].startRequirements[QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount].type = QUEST_REQUIREMENT_TYPE_QUEST;
                QUEST_INFOS[QUEST_INFO_COUNT].startRequirements[QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount].quest.state = 0;
            } else {
                if (QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount == ctx->reqCapacity) {
                    QUEST_INFOS[QUEST_INFO_COUNT].endRequirements = realloc(QUEST_INFOS[QUEST_INFO_COUNT].endRequirements, ctx->reqCapacity * 2 * sizeof(struct QuestRequirement));
                    ctx->reqCapacity *= 2;
                }
                QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount].type = QUEST_REQUIREMENT_TYPE_QUEST;
                QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount].quest.state = 0;
            }

            struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_CHECK_ITEM_TYPE_REQ_QUEST;
            ctx->head  = new;
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_QUEST: {
            if (strcmp(name, "int"))
                assert(0);

            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            ctx->skip++;
            if (!strcmp(key, "id")) {
                if (ctx->head->next->next->type == QUEST_CHECK_ITEM_TYPE_START)
                    QUEST_INFOS[QUEST_INFO_COUNT].startRequirements[QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount].quest.id = strtol(value, NULL, 10);
                else
                    QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount].quest.id = strtol(value, NULL, 10);
            } else if (!strcmp(key, "state")) {
                if (ctx->head->next->next->type == QUEST_CHECK_ITEM_TYPE_START)
                    QUEST_INFOS[QUEST_INFO_COUNT].startRequirements[QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount].quest.state = strtol(value, NULL, 10);
                else
                    QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount].quest.state = strtol(value, NULL, 10);
            }
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_ITEMS: {
            struct QuestInfo *quest = &QUEST_INFOS[QUEST_INFO_COUNT];
             if (strcmp(name, "imgdir"))
                assert(0);

            if (ctx->head->next->type == QUEST_CHECK_ITEM_TYPE_START)  {
                if (quest->startRequirementCount == ctx->reqCapacity) {
                    quest->startRequirements = realloc(quest->startRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                    ctx->reqCapacity *= 2;
                }

                quest->startRequirements[quest->startRequirementCount].type = QUEST_REQUIREMENT_TYPE_ITEM;
                quest->startRequirements[quest->startRequirementCount].item.count = 0;
            } else {
                if (quest->endRequirementCount == ctx->reqCapacity) {
                    quest->endRequirements = realloc(quest->endRequirements, (ctx->reqCapacity * 2) * sizeof(struct QuestRequirement));
                    ctx->reqCapacity *= 2;
                }

                quest->endRequirements[quest->endRequirementCount].type = QUEST_REQUIREMENT_TYPE_ITEM;
                quest->endRequirements[quest->endRequirementCount].item.count = 0;
            }


            struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_CHECK_ITEM_TYPE_REQ_ITEM;
            ctx->head = new;
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_ITEM: {
            struct QuestInfo *quest = &QUEST_INFOS[QUEST_INFO_COUNT];
             if (strcmp(name, "int"))
                assert(0);

            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            ctx->skip++;
            if (!strcmp(key, "id")) {
                if (ctx->head->next->next->type == QUEST_CHECK_ITEM_TYPE_START)
                    quest->startRequirements[quest->startRequirementCount].item.id = strtol(value, NULL, 10);
                else
                    quest->endRequirements[quest->endRequirementCount].item.id = strtol(value, NULL, 10);
            } else if (!strcmp(key, "count")) {
                if (ctx->head->next->next->type == QUEST_CHECK_ITEM_TYPE_START)
                    quest->startRequirements[quest->startRequirementCount].item.count = strtol(value, NULL, 10);
                else
                    quest->endRequirements[quest->endRequirementCount].item.count = strtol(value, NULL, 10);
            }
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_MOBS: {
            if (strcmp(name, "imgdir"))
                assert(0);

            struct QuestRequirement *req = ctx->head->next->type == QUEST_CHECK_ITEM_TYPE_START ?
                &QUEST_INFOS[QUEST_INFO_COUNT].startRequirements[QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount] :
                &QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount];

            if (req->mob.count == ctx->capacity) {
                req->mob.mobs = realloc(req->mob.mobs, (ctx->capacity * 2) * sizeof(*req->mob.mobs));
                ctx->capacity *= 2;
            }

            struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_CHECK_ITEM_TYPE_REQ_MOB;
            ctx->head = new;
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_MOB: {
             if (strcmp(name, "int"))
                assert(0);

            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            struct QuestRequirement *req = ctx->head->next->type == QUEST_CHECK_ITEM_TYPE_START ?
                &QUEST_INFOS[QUEST_INFO_COUNT].startRequirements[QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount] :
                &QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount];

            ctx->skip++;
            if (!strcmp(key, "id"))
                req->mob.mobs[req->mob.count].id = strtol(value, NULL, 10);
            else if (!strcmp(key, "count"))
                req->mob.mobs[req->mob.count].count = strtol(value, NULL, 10);
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_JOB: {
            if (strcmp(name, "int"))
                assert(0);

            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            struct QuestRequirement *req = ctx->head->next->type == QUEST_CHECK_ITEM_TYPE_START ?
                &QUEST_INFOS[QUEST_INFO_COUNT].startRequirements[QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount] :
                &QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount];

            ctx->skip++;
            if (req->job.count == ctx->capacity) {
                req->job.jobs = realloc(req->job.jobs, (ctx->capacity * 2) * sizeof(uint16_t));
                ctx->capacity *= 2;
            }

            req->job.jobs[req->job.count] = strtol(value, NULL, 10);
            req->job.count++;
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_INFOS: {
            struct QuestRequirement *req = ctx->infoReq;

            if (strcmp(name, "imgdir"))
                assert(0);

            if (req->info.infoCount == ctx->capacity) {
                req->info.infos = realloc(req->info.infos, (ctx->capacity * 2) * sizeof(char *));
                ctx->capacity *= 2;
            }

            struct QuestCheckParserStackNode *new = malloc(sizeof(struct QuestCheckParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_CHECK_ITEM_TYPE_REQ_INFO;
            ctx->head = new;
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_REQ_INFO: {
            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            struct QuestRequirement *req = ctx->infoReq;

            ctx->skip++;
            if (!strcmp(key, "value")) {
                req->info.infos[req->info.infoCount] = malloc(strlen(value) + 1);
                strcpy(req->info.infos[req->info.infoCount], value);
            }
        }
        break;

        default:
            ctx->skip++;
        }
    }
}

static void on_quest_check_end(void *user_data, const XML_Char *name)
{
    struct QuestCheckParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == QUEST_CHECK_ITEM_TYPE_REQ_INFO) {
        ctx->infoReq->info.infoCount++;
    } else if (ctx->head->type == QUEST_CHECK_ITEM_TYPE_REQ_MOB) {
        QUEST_INFOS[QUEST_INFO_COUNT].endRequirements[QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount].mob.count++;
    } else if (ctx->head->type == QUEST_CHECK_ITEM_TYPE_REQ_QUEST || ctx->head->type == QUEST_CHECK_ITEM_TYPE_REQ_ITEM || ctx->head->type == QUEST_CHECK_ITEM_TYPE_REQ_MOBS) {
        if (ctx->head->next->next->type == QUEST_CHECK_ITEM_TYPE_START)
            QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount++;
        else
            QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount++;
    } else if (ctx->head->type == QUEST_CHECK_ITEM_TYPE_REQ_JOB) {
        if (ctx->head->next->type == QUEST_CHECK_ITEM_TYPE_START)
            QUEST_INFOS[QUEST_INFO_COUNT].startRequirementCount++;
        else
            QUEST_INFOS[QUEST_INFO_COUNT].endRequirementCount++;
    } else if (ctx->head->type == QUEST_CHECK_ITEM_TYPE_QUEST) {
        QUEST_INFO_COUNT++;
    }

    struct QuestCheckParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_quest_act_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct QuestActParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        ctx->head = malloc(sizeof(struct QuestActParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = QUEST_ACT_ITEM_TYPE_TOP_LEVEL;
        if (strcmp(name, "imgdir"))
            assert(0); // ERROR
    } else {
        switch (ctx->head->type) {
        case QUEST_ACT_ITEM_TYPE_TOP_LEVEL: {
            if (strcmp(name, "imgdir"))
                assert(0); // ERROR

            uint16_t id = strtol(attrs[1], NULL, 10);
            ctx->currentQuest = cmph_search(QUEST_INFO_MPH, (void *)&id, sizeof(uint16_t));
            if (ctx->currentQuest >= QUEST_INFO_COUNT || QUEST_INFOS[ctx->currentQuest].id != id) {
                if (ctx->questCapacity == QUEST_INFO_COUNT) {
                    QUEST_INFOS = realloc(QUEST_INFOS, (ctx->questCapacity * 2) * sizeof(struct QuestInfo));
                    ctx->questCapacity *= 2;
                }

                ctx->currentQuest = QUEST_INFO_COUNT;
                QUEST_INFO_COUNT++;

                QUEST_INFOS[ctx->currentQuest].id = id;
                QUEST_INFOS[ctx->currentQuest].startRequirementCount = 0;
                QUEST_INFOS[ctx->currentQuest].startRequirements = NULL;
                QUEST_INFOS[ctx->currentQuest].endRequirementCount = 0;
                QUEST_INFOS[ctx->currentQuest].endRequirements = NULL;
                QUEST_INFOS[ctx->currentQuest].startActCount = 0;
                QUEST_INFOS[ctx->currentQuest].startActs = NULL;
                QUEST_INFOS[ctx->currentQuest].endActCount = 0;
                QUEST_INFOS[ctx->currentQuest].endActs = NULL;
                QUEST_INFOS[ctx->currentQuest].startScript = false;
                QUEST_INFOS[ctx->currentQuest].endScript = false;
            }

            struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_ACT_ITEM_TYPE_QUEST;
            ctx->head = new;
        }
        break;

        case QUEST_ACT_ITEM_TYPE_QUEST: {
            struct QuestInfo *quest = &QUEST_INFOS[ctx->currentQuest];
            if (strcmp(name, "imgdir"))
                assert(0);

            if (strcmp(attrs[0], "name"))
                assert(0);

            struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
            new->next = ctx->head;
            ctx->head = new;
            long t = strtol(attrs[1], NULL, 10);
            if (t == 0) {
                quest->startActs = malloc(sizeof(struct QuestAct));
                quest->startActCount = 0;
                ctx->actCapacity = 1;
                new->type = QUEST_ACT_ITEM_TYPE_START;
            } else {
                quest->endActs = malloc(sizeof(struct QuestAct));
                quest->endActCount = 0;
                ctx->actCapacity = 1;
                new->type = QUEST_ACT_ITEM_TYPE_END;
            }
        }
        break;

        case QUEST_ACT_ITEM_TYPE_START: {
            struct QuestInfo *quest = &QUEST_INFOS[ctx->currentQuest];
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (quest->startActCount == ctx->actCapacity) {
                    quest->startActs = realloc(quest->startActs, (ctx->actCapacity * 2) * sizeof(struct QuestAct));
                    ctx->actCapacity *= 2;
                }
                struct QuestAct *act = &quest->startActs[quest->startActCount];
                if (!strcmp(key, "money")) {
                    act->type = QUEST_ACT_TYPE_MESO;
                    act->meso.amount = strtol(value, NULL, 10);
                    quest->startActCount++;
                } else if (!strcmp(key, "exp")) {
                    act->type = QUEST_ACT_TYPE_EXP;
                    act->exp.amount = strtol(value, NULL, 10);
                    quest->startActCount++;
                } else if (!strcmp(key, "fame")) {
                    act->type = QUEST_ACT_TYPE_FAME;
                    act->fame.amount = strtol(value, NULL, 10);
                    quest->startActCount++;
                } else if (!strcmp(key, "nextQuest")) {
                    act->type = QUEST_ACT_TYPE_NEXT_QUEST;
                    act->nextQuest.qid = strtol(value, NULL, 10);
                    quest->startActCount++;
                }
                ctx->skip++;
            } else if (!strcmp(name, "string")) {
                ctx->skip++;
            } else if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "item")) {
                    if (quest->startActCount == ctx->actCapacity) {
                        quest->startActs = realloc(quest->startActs, (ctx->actCapacity * 2) * sizeof(struct QuestAct));
                        ctx->actCapacity *= 2;
                    }

                    quest->startActs[quest->startActCount].type = QUEST_ACT_TYPE_ITEM;
                    quest->startActs[quest->startActCount].item.count = 0;
                    quest->startActs[quest->startActCount].item.items = malloc(sizeof(struct QuestItemAction));

                    ctx->itemCapacity = 1;

                    struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_ACT_ITEM_TYPE_ACT_ITEMS;
                    ctx->head = new;
                } else if (!strcmp(attrs[1], "quest")) {
                    ctx->skip++;
                    //struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
                    //new->next = ctx->head;
                    //new->type = QUEST_ACT_ITEM_TYPE_ACT_QUESTS;
                    //ctx->head = new;
                } else {
                    ctx->skip++;
                }
            }
        }
        break;

        case QUEST_CHECK_ITEM_TYPE_END: {
            struct QuestInfo *quest = &QUEST_INFOS[ctx->currentQuest];
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (quest->endActCount == ctx->actCapacity) {
                    quest->endActs = realloc(quest->endActs, (ctx->actCapacity * 2) * sizeof(struct QuestAct));
                    ctx->actCapacity *= 2;
                }
                struct QuestAct *act = &quest->endActs[quest->endActCount];
                if (!strcmp(key, "money")) {
                    act->type = QUEST_ACT_TYPE_MESO;
                    act->meso.amount = strtol(value, NULL, 10);
                    quest->endActCount++;
                } else if (!strcmp(key, "exp")) {
                    act->type = QUEST_ACT_TYPE_EXP;
                    act->exp.amount = strtol(value, NULL, 10);
                    quest->endActCount++;
                } else if (!strcmp(key, "fame")) {
                    act->type = QUEST_ACT_TYPE_FAME;
                    act->fame.amount = strtol(value, NULL, 10);
                    quest->endActCount++;
                } else if (!strcmp(key, "nextQuest")) {
                    act->type = QUEST_ACT_TYPE_NEXT_QUEST;
                    act->nextQuest.qid = strtol(value, NULL, 10);
                    quest->endActCount++;
                }
                ctx->skip++;
            } else if (!strcmp(name, "string")) {
                ctx->skip++;
            } else if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "item")) {
                    if (quest->endActCount == ctx->actCapacity) {
                        quest->endActs = realloc(quest->endActs, ctx->actCapacity * 2 * sizeof(struct QuestAct));
                        ctx->actCapacity *= 2;
                    }

                    quest->endActs[quest->endActCount].type = QUEST_ACT_TYPE_ITEM;
                    quest->endActs[quest->endActCount].item.count = 0;
                    quest->endActs[quest->endActCount].item.items = malloc(sizeof(struct QuestItemAction));

                    ctx->itemCapacity = 1;

                    struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
                    new->next = ctx->head;
                    new->type = QUEST_ACT_ITEM_TYPE_ACT_ITEMS;
                    ctx->head = new;
                } else if (!strcmp(attrs[1], "quest")) {
                    ctx->skip++;
                    //struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
                    //new->next = ctx->head;
                    //new->type = QUEST_ACT_ITEM_TYPE_ACT_QUESTS;
                    //ctx->head = new;
                } else {
                    ctx->skip++;
                }
            }
        }
        break;

        case QUEST_ACT_ITEM_TYPE_ACT_QUESTS: {
            if (strcmp(name, "imgdir"))
                assert(0);

            struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_ACT_ITEM_TYPE_ACT_QUEST;
            ctx->head  = new;
        }
        break;

        case QUEST_ACT_ITEM_TYPE_ACT_QUEST: {
            struct QuestInfo *quest = &QUEST_INFOS[ctx->currentQuest];
            if (strcmp(name, "int"))
                assert(0);

            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            struct QuestAct *act = ctx->head->next->next->type == QUEST_ACT_ITEM_TYPE_START ?
                &quest->startActs[quest->startActCount] :
                &quest->endActs[quest->endActCount];

            ctx->skip++;
            if (!strcmp(key, "id"))
                act->quest.qid = strtol(value, NULL, 10);
            else if (!strcmp(key, "state"))
                act->quest.state = strtol(value, NULL, 10);
        }
        break;

        case QUEST_ACT_ITEM_TYPE_ACT_ITEMS: {
            if (strcmp(name, "imgdir"))
                assert(0);

            struct QuestAct *act = ctx->head->next->type == QUEST_ACT_ITEM_TYPE_START ?
                &QUEST_INFOS[ctx->currentQuest].startActs[QUEST_INFOS[ctx->currentQuest].startActCount] :
                &QUEST_INFOS[ctx->currentQuest].endActs[QUEST_INFOS[ctx->currentQuest].endActCount];

            if (ctx->itemCapacity == act->item.count) {
                act->item.items = realloc(act->item.items, (ctx->itemCapacity * 2) * sizeof(struct QuestItemAction));
                ctx->itemCapacity *= 2;
            }

            act->type = QUEST_ACT_TYPE_ITEM;
            act->item.items[act->item.count].gender = 0;
            act->item.items[act->item.count].job = 0;
            act->item.items[act->item.count].prop = 0;
            act->item.items[act->item.count].period = 0;
            act->item.items[act->item.count].var = 0;

            struct QuestActParserStackNode *new = malloc(sizeof(struct QuestActParserStackNode));
            new->next = ctx->head;
            new->type = QUEST_ACT_ITEM_TYPE_ACT_ITEM;
            ctx->head  = new;
        }
        break;

        case QUEST_ACT_ITEM_TYPE_ACT_ITEM: {
            const XML_Char *key = NULL;
            const XML_Char *value = NULL;
            for (size_t i = 0; attrs[i] != NULL; i += 2) {
                if (!strcmp(attrs[i], "name"))
                    key = attrs[i+1];
                else if (!strcmp(attrs[i], "value"))
                    value = attrs[i+1];
            }

            assert(key != NULL && value != NULL);

            struct QuestAct *act = ctx->head->next->next->type == QUEST_ACT_ITEM_TYPE_START ?
                &QUEST_INFOS[ctx->currentQuest].startActs[QUEST_INFOS[ctx->currentQuest].startActCount] :
                &QUEST_INFOS[ctx->currentQuest].endActs[QUEST_INFOS[ctx->currentQuest].endActCount];

            ctx->skip++;
            if (!strcmp(name, "int")) {
                if (!strcmp(key, "id")) {
                    act->item.items[act->item.count].id = strtol(value, NULL, 10);
                } else if (!strcmp(key, "count")) {
                    act->item.items[act->item.count].count = strtol(value, NULL, 10);
                } else if (!strcmp(key, "gender")) {
                    act->item.items[act->item.count].gender = strtol(value, NULL, 10);
                } else if (!strcmp(key, "period")) {
                    act->item.items[act->item.count].period = strtol(value, NULL, 10);
                } else if (!strcmp(key, "prop")) {
                    act->item.items[act->item.count].prop = strtol(value, NULL, 10);
                } else if (!strcmp(key, "var")) {
                    act->item.items[act->item.count].var = strtol(value, NULL, 10);
                } else if (!strcmp(key, "job")) {
                    act->item.items[act->item.count].job = strtol(value, NULL, 10);
                }
            }
            // TODO: dateExpire
        }
        break;

        default:
            ctx->skip++;
        }
    }
}

static void on_quest_act_end(void *user_data, const XML_Char *name)
{
    struct QuestActParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == QUEST_ACT_ITEM_TYPE_ACT_ITEM) {
        if (ctx->head->next->next->type == QUEST_ACT_ITEM_TYPE_START)
            QUEST_INFOS[ctx->currentQuest].startActs[QUEST_INFOS[ctx->currentQuest].startActCount].item.count++;
        else
            QUEST_INFOS[ctx->currentQuest].endActs[QUEST_INFOS[ctx->currentQuest].endActCount].item.count++;
    } else if (ctx->head->type == QUEST_ACT_ITEM_TYPE_ACT_QUESTS || ctx->head->type == QUEST_ACT_ITEM_TYPE_ACT_ITEMS) {
        if (ctx->head->next->type == QUEST_ACT_ITEM_TYPE_START)
            QUEST_INFOS[ctx->currentQuest].startActCount++;
        else
            QUEST_INFOS[ctx->currentQuest].endActCount++;
    }

    struct QuestActParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_item_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct ItemParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        ctx->head = malloc(sizeof(struct ItemParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = ITEM_ITEM_TYPE_TOP_LEVEL;
        if (strcmp(name, "imgdir"))
            assert(0); // ERROR
    } else {
        switch (ctx->head->type) {
        case ITEM_ITEM_TYPE_TOP_LEVEL:
            if (strcmp(name, "imgdir"))
                assert(0); // ERROR

            if (ITEM_INFO_COUNT == ctx->itemCapacity) {
                ITEM_INFOS = realloc(ITEM_INFOS, (ctx->itemCapacity * 2) * sizeof(struct ItemInfo));
                ctx->itemCapacity *= 2;
            }

            ITEM_INFOS[ITEM_INFO_COUNT].id = strtol(attrs[1], NULL, 10);
            ITEM_INFOS[ITEM_INFO_COUNT].slotMax = 100;
            ITEM_INFOS[ITEM_INFO_COUNT].price = 0;
            ITEM_INFOS[ITEM_INFO_COUNT].unitPrice = 0;
            ITEM_INFOS[ITEM_INFO_COUNT].untradable = false;
            ITEM_INFOS[ITEM_INFO_COUNT].oneOfAKind = false;
            ITEM_INFOS[ITEM_INFO_COUNT].monsterBook = false;
            struct ItemParserStackNode *new = malloc(sizeof(struct ItemParserStackNode));
            new->next = ctx->head;
            new->type = ITEM_ITEM_TYPE_ITEM;
            ctx->head = new;
        break;

        case ITEM_ITEM_TYPE_ITEM:
            if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "info")) {
                    struct ItemParserStackNode *new = malloc(sizeof(struct ItemParserStackNode));
                    new->next = ctx->head;
                    new->type = ITEM_ITEM_TYPE_INFO;
                    ctx->head = new;
                } else {
                    ctx->skip++;
                }
            } else {
                ctx->skip++;
            }
        break;

        case ITEM_ITEM_TYPE_INFO: {
            ctx->skip++;
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (!strcmp(key, "slotMax"))
                    ITEM_INFOS[ITEM_INFO_COUNT].slotMax = strtol(value, NULL, 10);
                else if (!strcmp(key, "price"))
                    ITEM_INFOS[ITEM_INFO_COUNT].price = strtol(value, NULL, 10);
                else if (!strcmp(key, "tradeBlock"))
                    ITEM_INFOS[ITEM_INFO_COUNT].untradable = strtol(value, NULL, 10) > 0;
                else if (!strcmp(key, "only"))
                    ITEM_INFOS[ITEM_INFO_COUNT].oneOfAKind = strtol(value, NULL, 10) > 0;
                else if (!strcmp(key, "monsterBook"))
                    ITEM_INFOS[ITEM_INFO_COUNT].monsterBook = strtol(value, NULL, 10) > 0;
            } else if (!strcmp(name, "double")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (!strcmp(key, "unitPrice")) {
                    XML_Char *dup = strdup(value);
                    if (strchr(dup, ',') != NULL)
                        *strchr(dup, ',') = '.';
                    ITEM_INFOS[ITEM_INFO_COUNT].unitPrice = strtod(dup, NULL);
                    free(dup);
                }
            }
        }
        break;
        }
    }
}

static void on_item_end(void *user_data, const XML_Char *name)
{
    struct ItemParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == ITEM_ITEM_TYPE_ITEM)
        ITEM_INFO_COUNT++;

    struct ItemParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_equip_item_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct ItemParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head2 == NULL) {
        if (ITEM_INFO_COUNT == ctx->itemCapacity) {
            ITEM_INFOS = realloc(ITEM_INFOS, (ctx->itemCapacity * 2) * sizeof(struct ItemInfo));
            ctx->itemCapacity *= 2;
        }

        ITEM_INFOS[ITEM_INFO_COUNT].id = strtol(attrs[1], NULL, 10);
        ITEM_INFOS[ITEM_INFO_COUNT].slotMax = 100;
        ITEM_INFOS[ITEM_INFO_COUNT].price = 0;
        ITEM_INFOS[ITEM_INFO_COUNT].unitPrice = 0;
        ITEM_INFOS[ITEM_INFO_COUNT].untradable = false;
        ITEM_INFOS[ITEM_INFO_COUNT].oneOfAKind = false;
        ITEM_INFOS[ITEM_INFO_COUNT].monsterBook = false;

        ctx->head2 = malloc(sizeof(struct EquipItemParserStackNode));
        ctx->head2->next = NULL;
        ctx->head2->type = EQUIP_ITEM_ITEM_TYPE_TOP_LEVEL;
        if (strcmp(name, "imgdir"))
            assert(0); // ERROR
    } else {
        switch (ctx->head2->type) {
        case EQUIP_ITEM_ITEM_TYPE_TOP_LEVEL:
            if (!strcmp(attrs[1], "info")) {
                struct EquipItemParserStackNode *new = malloc(sizeof(struct EquipItemParserStackNode));
                new->next = ctx->head2;
                new->type = EQUIP_ITEM_ITEM_TYPE_INFO;
                ctx->head2 = new;
            } else {
                ctx->skip++;
            }
        break;

        case EQUIP_ITEM_ITEM_TYPE_INFO: {
            ctx->skip++;
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (!strcmp(key, "slotMax"))
                    ITEM_INFOS[ITEM_INFO_COUNT].slotMax = strtol(value, NULL, 10);
                else if (!strcmp(key, "price"))
                    ITEM_INFOS[ITEM_INFO_COUNT].price = strtol(value, NULL, 10);
                else if (!strcmp(key, "tradeBlock"))
                    ITEM_INFOS[ITEM_INFO_COUNT].untradable = strtol(value, NULL, 10) > 0;
                else if (!strcmp(key, "only"))
                    ITEM_INFOS[ITEM_INFO_COUNT].oneOfAKind = strtol(value, NULL, 10) > 0;
                else if (!strcmp(key, "monsterBook"))
                    ITEM_INFOS[ITEM_INFO_COUNT].monsterBook = strtol(value, NULL, 10) > 0;
            } else if (!strcmp(name, "double")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (!strcmp(key, "unitPrice")) {
                    XML_Char *dup = strdup(value);
                    if (strchr(dup, ',') != NULL)
                        *strchr(dup, ',') = '.';
                    ITEM_INFOS[ITEM_INFO_COUNT].unitPrice = strtod(dup, NULL);
                    free(dup);
                }
            }
        }
        break;
        }
    }
}

static void on_equip_item_end(void *user_data, const XML_Char *name)
{
    struct ItemParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head2->type == EQUIP_ITEM_ITEM_TYPE_INFO) {
        free(ctx->head2->next);
        free(ctx->head2);
        ctx->head2 = NULL;
        XML_StopParser(ctx->parser, false);
        ITEM_INFO_COUNT++;
        return;
    }

    struct EquipItemParserStackNode *next = ctx->head2->next;
    free(ctx->head2);
    ctx->head2 = next;
}

static void on_equip_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct EquipParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        assert(!strcmp(name, "imgdir"));

        EQUIP_INFOS[ctx->currentEquip].id = strtol(attrs[1], NULL, 10);
        EQUIP_INFOS[ctx->currentEquip].reqJob = 0;
        EQUIP_INFOS[ctx->currentEquip].reqLevel = 0;
        EQUIP_INFOS[ctx->currentEquip].reqStr = 0;
        EQUIP_INFOS[ctx->currentEquip].reqDex = 0;
        EQUIP_INFOS[ctx->currentEquip].reqInt = 0;
        EQUIP_INFOS[ctx->currentEquip].reqLuk = 0;
        EQUIP_INFOS[ctx->currentEquip].str = 0;
        EQUIP_INFOS[ctx->currentEquip].dex = 0;
        EQUIP_INFOS[ctx->currentEquip].int_ = 0;
        EQUIP_INFOS[ctx->currentEquip].luk = 0;
        EQUIP_INFOS[ctx->currentEquip].hp = 0;
        EQUIP_INFOS[ctx->currentEquip].mp = 0;
        EQUIP_INFOS[ctx->currentEquip].atk = 0;
        EQUIP_INFOS[ctx->currentEquip].matk = 0;
        EQUIP_INFOS[ctx->currentEquip].def = 0;
        EQUIP_INFOS[ctx->currentEquip].mdef = 0;
        EQUIP_INFOS[ctx->currentEquip].acc = 0;
        EQUIP_INFOS[ctx->currentEquip].avoid = 0;
        EQUIP_INFOS[ctx->currentEquip].speed = 0;
        EQUIP_INFOS[ctx->currentEquip].jump = 0;
        EQUIP_INFOS[ctx->currentEquip].slots = 0;
        EQUIP_INFOS[ctx->currentEquip].cash = false;

        ctx->head = malloc(sizeof(struct EquipParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = EQUIP_ITEM_TYPE_TOP_LEVEL;
    } else {
        switch (ctx->head->type) {
        case EQUIP_ITEM_TYPE_TOP_LEVEL:
            if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "info")) {
                    struct EquipParserStackNode *new = malloc(sizeof(struct EquipParserStackNode));
                    new->next = ctx->head;
                    new->type = EQUIP_ITEM_TYPE_INFO;
                    ctx->head = new;
                } else {
                    ctx->skip++;
                }
            } else {
                ctx->skip++;
            }
        break;

        case EQUIP_ITEM_TYPE_INFO:
            ctx->skip++;
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (!strcmp(key, "reqJob"))
                    EQUIP_INFOS[ctx->currentEquip].reqJob = strtol(value, NULL, 10);
                else if (!strcmp(key, "reqLevel"))
                    EQUIP_INFOS[ctx->currentEquip].reqLevel = strtol(value, NULL, 10);
                else if (!strcmp(key, "reqSTR"))
                    EQUIP_INFOS[ctx->currentEquip].reqStr = strtol(value, NULL, 10);
                else if (!strcmp(key, "reqDEX"))
                    EQUIP_INFOS[ctx->currentEquip].reqDex = strtol(value, NULL, 10);
                else if (!strcmp(key, "reqINT"))
                    EQUIP_INFOS[ctx->currentEquip].reqInt = strtol(value, NULL, 10);
                else if (!strcmp(key, "reqLUK"))
                    EQUIP_INFOS[ctx->currentEquip].reqLuk = strtol(value, NULL, 10);
                else if (!strcmp(key, "incSTR"))
                    EQUIP_INFOS[ctx->currentEquip].str = strtol(value, NULL, 10);
                else if (!strcmp(key, "incDEX"))
                    EQUIP_INFOS[ctx->currentEquip].dex = strtol(value, NULL, 10);
                else if (!strcmp(key, "incINT"))
                    EQUIP_INFOS[ctx->currentEquip].int_ = strtol(value, NULL, 10);
                else if (!strcmp(key, "incLUK"))
                    EQUIP_INFOS[ctx->currentEquip].luk = strtol(value, NULL, 10);
                else if (!strcmp(key, "incMHP"))
                    EQUIP_INFOS[ctx->currentEquip].hp = strtol(value, NULL, 10);
                else if (!strcmp(key, "incMMP"))
                    EQUIP_INFOS[ctx->currentEquip].mp = strtol(value, NULL, 10);
                else if (!strcmp(key, "incLUK"))
                    EQUIP_INFOS[ctx->currentEquip].luk = strtol(value, NULL, 10);
                else if (!strcmp(key, "incPAD"))
                    EQUIP_INFOS[ctx->currentEquip].atk = strtol(value, NULL, 10);
                else if (!strcmp(key, "incMAD"))
                    EQUIP_INFOS[ctx->currentEquip].matk = strtol(value, NULL, 10);
                else if (!strcmp(key, "incPDD"))
                    EQUIP_INFOS[ctx->currentEquip].def = strtol(value, NULL, 10);
                else if (!strcmp(key, "incMDD"))
                    EQUIP_INFOS[ctx->currentEquip].mdef = strtol(value, NULL, 10);
                else if (!strcmp(key, "incACC"))
                    EQUIP_INFOS[ctx->currentEquip].acc = strtol(value, NULL, 10);
                else if (!strcmp(key, "incEVA"))
                    EQUIP_INFOS[ctx->currentEquip].avoid = strtol(value, NULL, 10);
                else if (!strcmp(key, "incSpeed"))
                    EQUIP_INFOS[ctx->currentEquip].speed = strtol(value, NULL, 10);
                else if (!strcmp(key, "incJump"))
                    EQUIP_INFOS[ctx->currentEquip].jump = strtol(value, NULL, 10);
                else if (!strcmp(key, "tuc"))
                    EQUIP_INFOS[ctx->currentEquip].slots = strtol(value, NULL, 10);
                else if (!strcmp(key, "attackSpeed"))
                    EQUIP_INFOS[ctx->currentEquip].attackSpeed = strtol(value, NULL, 10);
                else if (!strcmp(key, "cash"))
                    EQUIP_INFOS[ctx->currentEquip].cash = strtol(value, NULL, 10) > 0;
            }
        break;
        }
    }
}

static void on_equip_end(void *user_data, const XML_Char *name)
{
    struct EquipParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == EQUIP_ITEM_TYPE_INFO) {
        free(ctx->head->next);
        free(ctx->head);
        ctx->head = NULL;
        XML_StopParser(ctx->parser, false);
        return;
    }

    struct EquipParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_consumable_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct ConsumableParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        ctx->head = malloc(sizeof(struct ConsumableParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = CONSUMABLE_ITEM_TYPE_TOP_LEVEL;
        assert(!strcmp(name, "imgdir"));
    } else {
        switch (ctx->head->type) {
        case CONSUMABLE_ITEM_TYPE_TOP_LEVEL:
            assert(!strcmp(name, "imgdir"));

            if (CONSUMABLE_INFO_COUNT == ctx->itemCapacity) {
                CONSUMABLE_INFOS = realloc(CONSUMABLE_INFOS, (ctx->itemCapacity * 2) * sizeof(struct ConsumableInfo));
                ctx->itemCapacity *= 2;
            }

            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].id = strtol(attrs[1], NULL, 10);
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].hp = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].mp = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].hpR = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].mpR = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].atk = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].matk = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].def = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].mdef = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].acc = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].avoid = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].speed = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].jump = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].time = 0;
            CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].consumeOnPickup = false;
            struct ConsumableParserStackNode *new = malloc(sizeof(struct ConsumableParserStackNode));
            new->next = ctx->head;
            new->type = CONSUMABLE_ITEM_TYPE_ITEM;
            ctx->head = new;
        break;

        case CONSUMABLE_ITEM_TYPE_ITEM:
            if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "spec")) {
                    struct ConsumableParserStackNode *new = malloc(sizeof(struct ConsumableParserStackNode));
                    new->next = ctx->head;
                    new->type = CONSUMABLE_ITEM_TYPE_SPEC;
                    ctx->head = new;
                } else {
                    ctx->skip++;
                }
            } else {
                ctx->skip++;
            }
        break;

        case CONSUMABLE_ITEM_TYPE_SPEC: {
            ctx->skip++;
            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                if (!strcmp(key, "hp"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].hp = strtol(value, NULL, 10);
                else if (!strcmp(key, "mp"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].mp = strtol(value, NULL, 10);
                else if (!strcmp(key, "hpR"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].hpR = strtol(value, NULL, 10);
                else if (!strcmp(key, "mpR"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].mpR = strtol(value, NULL, 10);
                else if (!strcmp(key, "pad"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].atk = strtol(value, NULL, 10);
                else if (!strcmp(key, "mad"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].matk = strtol(value, NULL, 10);
                else if (!strcmp(key, "pdd"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].def = strtol(value, NULL, 10);
                else if (!strcmp(key, "mdd"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].mdef = strtol(value, NULL, 10);
                else if (!strcmp(key, "acc"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].acc = strtol(value, NULL, 10);
                else if (!strcmp(key, "eva"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].avoid = strtol(value, NULL, 10);
                else if (!strcmp(key, "speed"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].speed = strtol(value, NULL, 10);
                else if (!strcmp(key, "jump"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].jump = strtol(value, NULL, 10);
                else if (!strcmp(key, "time"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].time = strtol(value, NULL, 10);
                else if (!strcmp(key, "consumeOnPickup"))
                    CONSUMABLE_INFOS[CONSUMABLE_INFO_COUNT].consumeOnPickup = strtol(value, NULL, 10) > 0;
            }
        }
        break;
        }
    }
}

static void on_consumable_end(void *user_data, const XML_Char *name)
{
    struct ConsumableParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == CONSUMABLE_ITEM_TYPE_ITEM)
        CONSUMABLE_INFO_COUNT++;

    struct ConsumableParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_reactor_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct ReactorParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        assert(!strcmp(name, "imgdir"));

        ctx->stateCapacity = 1;
        REACTOR_INFOS[REACTOR_INFO_COUNT].id = strtol(attrs[1], NULL, 10);
        REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount = 0;
        REACTOR_INFOS[REACTOR_INFO_COUNT].states = malloc(sizeof(struct ReactorStateInfo));

        ctx->head = malloc(sizeof(struct ReactorParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = REACTOR_ITEM_TYPE_TOP_LEVEL;
    } else {
        switch (ctx->head->type) {
        case REACTOR_ITEM_TYPE_TOP_LEVEL:
            if (!strcmp(name, "string")) {
                assert(!strcmp(attrs[0], "name"));
                assert(!strcmp(attrs[1], "action"));
                assert(!strcmp(attrs[2], "value"));
                ctx->skip++;
                strcpy(REACTOR_INFOS[REACTOR_INFO_COUNT].action, attrs[3]);
            } else if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "info")) {
                    struct ReactorParserStackNode *new = malloc(sizeof(struct ReactorParserStackNode));
                    new->next = ctx->head;
                    new->type = REACTOR_ITEM_TYPE_INFO;
                    ctx->head = new;
                } else {
                    if (REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount == ctx->stateCapacity) {
                        REACTOR_INFOS[REACTOR_INFO_COUNT].states = realloc(REACTOR_INFOS[REACTOR_INFO_COUNT].states, (ctx->stateCapacity * 2) * sizeof(struct ReactorStateInfo));
                        ctx->stateCapacity *= 2;
                    }

                    REACTOR_INFOS[REACTOR_INFO_COUNT].states[REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount].eventCount = 0;
                    REACTOR_INFOS[REACTOR_INFO_COUNT].states[REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount].events = NULL;

                    struct ReactorParserStackNode *new = malloc(sizeof(struct ReactorParserStackNode));
                    new->next = ctx->head;
                    new->type = REACTOR_ITEM_TYPE_STATE;
                    ctx->head = new;
                }
            } else {
                ctx->skip++; // TODO: <int name="quest">
            }
        break;

        case REACTOR_ITEM_TYPE_INFO:
            if (!strcmp(name, "string")) {
                const XML_Char *key = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                }

                assert(key != NULL);

                ctx->skip++;
                if (!strcmp(key, "link")) {
                    struct ReactorInfo *reactor = &REACTOR_INFOS[REACTOR_INFO_COUNT];
                    for (size_t i = 0; i < reactor->stateCount; i++) {
                        for (size_t j = 0; j < reactor->states[i].eventCount; j++) {
                            if (reactor->states[i].events[j].type == REACTOR_EVENT_TYPE_SKILL)
                                free(reactor->states[i].events[j].skills);
                        }
                        free(reactor->states[i].events);
                    }
                    free(reactor->states);
                    free(ctx->head->next);
                    free(ctx->head);
                    ctx->head = NULL;
                    XML_StopParser(ctx->parser, false);
                }
            } else {
                ctx->skip++;
            }

        break;

        case REACTOR_ITEM_TYPE_STATE:
            if (!strcmp(name, "imgdir")) {
                const XML_Char *key = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name")) {
                        key = attrs[i+1];
                        break;
                    }
                }

                assert(key != NULL);

                if (!strcmp(key, "event")) {
                    struct ReactorStateInfo *state = &REACTOR_INFOS[REACTOR_INFO_COUNT].states[REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount];

                    ctx->eventCapacity = 1;
                    state->events = malloc(sizeof(struct ReactorEventInfo));

                    struct ReactorParserStackNode *new = malloc(sizeof(struct ReactorParserStackNode));
                    new->next = ctx->head;
                    new->type = REACTOR_ITEM_TYPE_EVENTS;
                    ctx->head = new;
                } else {
                    ctx->skip++;
                }
            } else {
                ctx->skip++;
            }
        break;

        case REACTOR_ITEM_TYPE_EVENTS: {
            if (!strcmp(name, "imgdir")) {
                struct ReactorStateInfo *state = &REACTOR_INFOS[REACTOR_INFO_COUNT].states[REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount];

                if (state->eventCount == ctx->eventCapacity) {
                    state->events = realloc(state->events, (ctx->eventCapacity * 2) * sizeof(struct ReactorEventInfo));
                    ctx->eventCapacity *= 2;
                }

                struct ReactorParserStackNode *new = malloc(sizeof(struct ReactorParserStackNode));
                new->next = ctx->head;
                new->type = REACTOR_ITEM_TYPE_EVENT;
                ctx->head = new;
            } else {
                // TODO: Parse timeOut
                ctx->skip++;
            }
        }
        break;

        case REACTOR_ITEM_TYPE_EVENT: {
            struct ReactorStateInfo *state = &REACTOR_INFOS[REACTOR_INFO_COUNT].states[REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount];
            struct ReactorEventInfo *event = &state->events[state->eventCount];

            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                ctx->skip++;
                if (!strcmp(key, "type")) {
                    event->type = strtol(value, NULL, 10);
                } else if (!strcmp(key, "state")) {
                    event->next = strtol(value, NULL, 10);
                } else if (!strcmp(key, "0")) {
                    event->item = strtol(value, NULL, 10);
                } else if (!strcmp(key, "1")) {
                    event->count = strtol(value, NULL, 10);
                }
            } else if (!strcmp(name, "imgdir")) {
                const XML_Char *key = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                }

                assert(key != NULL);

                if (!strcmp(key, "activeSkillID")) {
                    ctx->skillCapacity = 1;
                    event->skills = malloc(sizeof(uint32_t));
                    event->skillCount = 0;

                    struct ReactorParserStackNode *new = malloc(sizeof(struct ReactorParserStackNode));
                    new->next = ctx->head;
                    new->type = REACTOR_ITEM_TYPE_SKILLS;
                    ctx->head = new;
                } else {
                    ctx->skip++;
                }
            } else {
                ctx->skip++;
            }
        }
        break;

        case REACTOR_ITEM_TYPE_SKILLS: {
            struct ReactorInfo *reactor = &REACTOR_INFOS[REACTOR_INFO_COUNT];
            struct ReactorStateInfo *state = &reactor->states[reactor->stateCount];
            struct ReactorEventInfo *event = &state->events[state->eventCount];

            if (event->skillCount == ctx->skillCapacity) {
                event->skills = realloc(event->skills, (ctx->skillCapacity * 2) * sizeof(uint32_t));
                ctx->skillCapacity *= 2;
            }

            if (!strcmp(name, "int")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];
                    else if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                ctx->skip++;
                event->skills[event->skillCount] = strtol(value, NULL, 10);
                event->skillCount++;
            } else {
                assert(0);
            }
        }
        break;
        }
    }
}

static void on_reactor_end(void *user_data, const XML_Char *name)
{
    struct ReactorParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == REACTOR_ITEM_TYPE_EVENT) {
        REACTOR_INFOS[REACTOR_INFO_COUNT].states[REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount].eventCount++;
    } else if (ctx->head->type == REACTOR_ITEM_TYPE_STATE) {
        REACTOR_INFOS[REACTOR_INFO_COUNT].stateCount++;
    } else if (ctx->head->type == REACTOR_ITEM_TYPE_TOP_LEVEL) {
        REACTOR_INFO_COUNT++;
    }

    struct ReactorParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;
}

static void on_reactor_second_pass_start(void *user_data, const XML_Char *name, const XML_Char **attrs)
{
    struct ReactorParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip++;
        return;
    }

    if (ctx->head == NULL) {
        assert(!strcmp(name, "imgdir"));

        ctx->isLink = false;
        REACTOR_INFOS[REACTOR_INFO_COUNT].id = strtol(attrs[1], NULL, 10);

        ctx->head = malloc(sizeof(struct ReactorParserStackNode));
        ctx->head->next = NULL;
        ctx->head->type = REACTOR_ITEM_TYPE_TOP_LEVEL;
    } else {
        switch (ctx->head->type) {
        case REACTOR_ITEM_TYPE_TOP_LEVEL:
            if (!strcmp(name, "string")) {
                assert(!strcmp(attrs[0], "name"));
                assert(!strcmp(attrs[1], "action"));
                assert(!strcmp(attrs[2], "value"));
                ctx->skip++;
                strcpy(REACTOR_INFOS[REACTOR_INFO_COUNT].action, attrs[3]);
            } else if (!strcmp(name, "imgdir")) {
                if (!strcmp(attrs[1], "info")) {
                    struct ReactorParserStackNode *new = malloc(sizeof(struct ReactorParserStackNode));
                    new->next = ctx->head;
                    new->type = REACTOR_ITEM_TYPE_INFO;
                    ctx->head = new;
                } else {
                    ctx->skip++;
                }
            } else {
                ctx->skip++;
            }
        break;

        case REACTOR_ITEM_TYPE_INFO:
            if (!strcmp(name, "string")) {
                const XML_Char *key = NULL;
                const XML_Char *value = NULL;
                for (size_t i = 0; attrs[i] != NULL; i += 2) {
                    if (!strcmp(attrs[i], "name"))
                        key = attrs[i+1];

                    if (!strcmp(attrs[i], "value"))
                        value = attrs[i+1];
                }

                assert(key != NULL && value != NULL);

                ctx->skip++;
                if (!strcmp(key, "link")) {
                    ctx->isLink = true;
                    struct ReactorInfo *reactor = &REACTOR_INFOS[REACTOR_INFO_COUNT];
                    uint32_t link = strtol(value, NULL, 10);
                    struct ReactorInfo *other = &REACTOR_INFOS[cmph_search(REACTOR_INFO_MPH, (void *)&link, sizeof(uint32_t))];
                    reactor->stateCount = other->stateCount;
                    reactor->states = malloc(other->stateCount * sizeof(struct ReactorStateInfo));
                    for (size_t i = 0; i < other->stateCount; i++) {
                        reactor->states[i].eventCount = other->states[i].eventCount;
                        reactor->states[i].events = malloc(other->states[i].eventCount * sizeof(struct ReactorEventInfo));
                        for (size_t j = 0; j < other->states[i].eventCount; j++) {
                            reactor->states[i].events[j] = other->states[i].events[j];
                            if (other->states[i].events[j].type == REACTOR_EVENT_TYPE_SKILL) {
                                reactor->states[i].events[j].skills = malloc(other->states[i].events[j].skillCount * sizeof(uint32_t));
                                for (size_t k = 0; k < other->states[i].events[j].skillCount; k++)
                                    reactor->states[i].events[j].skills[k] = other->states[i].events[j].skills[k];
                            }
                        }
                    }
                }
            } else {
                ctx->skip++;
            }
        break;

        default:
            ctx->skip++;
        }
    }

}

static void on_reactor_second_pass_end(void *user_data, const XML_Char *name)
{
    struct ReactorParserContext *ctx = user_data;
    if (ctx->skip > 0) {
        ctx->skip--;
        return;
    }

    if (ctx->head->type == REACTOR_ITEM_TYPE_INFO && !ctx->isLink) {
        // If we completed traversing the info section and haven't found a link then quit immediatly
        free(ctx->head->next);
        free(ctx->head);
        ctx->head = NULL;
        XML_StopParser(ctx->parser, false);
        return;
    } else if (ctx->head->type == REACTOR_ITEM_TYPE_TOP_LEVEL && ctx->isLink) {
        REACTOR_INFO_COUNT++;
    }

    struct ReactorParserStackNode *next = ctx->head->next;
    free(ctx->head);
    ctx->head = next;

}

static struct Rectangle foothold_mbr(struct Foothold *fh)
{
    struct Rectangle rect = {
        {
            MIN(fh->p1.x, fh->p2.x),
            MAX(fh->p1.y, fh->p2.y)
        },
        {
            MAX(fh->p1.x, fh->p2.x),
            MIN(fh->p1.y, fh->p2.y)
        },
    };

    return rect;
}

static struct Rectangle rectangle_mbr(struct Rectangle *r1, struct Rectangle *r2)
{
    struct Rectangle rect;
    rect.sw.x = MIN(r1->sw.x, r2->sw.x);
    rect.sw.y = MAX(r1->sw.y, r2->sw.y);
    rect.ne.x = MAX(r1->ne.x, r2->ne.x);
    rect.ne.y = MIN(r1->ne.y, r2->ne.y);
    return rect;
}

static struct Rectangle rectangle_intersection(struct Rectangle *r1, struct Rectangle *r2)
{
    struct Rectangle rect;
    rect.sw.x = MAX(r1->sw.x, r2->sw.x);
    rect.sw.y = MIN(r1->sw.y, r2->sw.y);
    rect.ne.x = MIN(r1->ne.x, r2->ne.x);
    rect.ne.y = MAX(r1->ne.y, r2->ne.y);
    if (rect.sw.x > rect.ne.x || rect.sw.y < rect.ne.y) {
        rect.sw.x = 0;
        rect.sw.y = 0;
        rect.ne.x = 0;
        rect.ne.y = 0;
    }
    return rect;
}

static int32_t rectangle_area(struct Rectangle *rect)
{
    return (rect->ne.x - rect->sw.x) * (rect->sw.y - rect->ne.y);
}

static int16_t rectangle_length(struct Rectangle *rect)
{
    return (rect->ne.x - rect->sw.x) + (rect->sw.y - rect->ne.y);
}

static int32_t white_space_area_2(struct Rectangle *r1, struct Rectangle *r2)
{
    struct Rectangle intersect1 = rectangle_intersection(r1, r2);
    int32_t cover_area = rectangle_area(r1) + rectangle_area(r2) - rectangle_area(&intersect1);
    struct Rectangle union_ = rectangle_mbr(r1, r2);
    return rectangle_area(&union_) - cover_area;
}

static int32_t white_space_length_2(struct Rectangle *r1, struct Rectangle *r2)
{
    struct Rectangle intersect1 = rectangle_intersection(r1, r2);
    int32_t cover_area = rectangle_length(r1) + rectangle_length(r2) - rectangle_length(&intersect1);
    struct Rectangle union_ = rectangle_mbr(r1, r2);
    return rectangle_length(&union_) - cover_area;
}

static int32_t white_space_area_3(struct Rectangle *r1, struct Rectangle *r2, struct Rectangle *r3)
{
    struct Rectangle intersect1 = rectangle_intersection(r1, r2);
    struct Rectangle intersect2 = rectangle_intersection(r1, r3);
    struct Rectangle intersect3 = rectangle_intersection(r2, r3);
    struct Rectangle intersect4 = rectangle_intersection(&intersect1, r3);
    int32_t cover_area = rectangle_area(r1) + rectangle_area(r2) + rectangle_area(r3) - rectangle_area(&intersect1) - rectangle_area(&intersect2) - rectangle_area(&intersect3) + rectangle_area(&intersect4);
    struct Rectangle union_ = rectangle_mbr(r1, r2);
    union_ = rectangle_mbr(&union_, r3);
    return rectangle_area(&union_) - cover_area;
}

static int16_t white_space_length_3(struct Rectangle *r1, struct Rectangle *r2, struct Rectangle *r3)
{
    struct Rectangle intersect1 = rectangle_intersection(r1, r2);
    struct Rectangle intersect2 = rectangle_intersection(r1, r3);
    struct Rectangle intersect3 = rectangle_intersection(r2, r3);
    struct Rectangle intersect4 = rectangle_intersection(&intersect1, r3);
    int32_t cover_area = rectangle_length(r1) + rectangle_length(r2) + rectangle_length(r3) - rectangle_length(&intersect1) - rectangle_length(&intersect2) - rectangle_length(&intersect3) + rectangle_length(&intersect4);
    struct Rectangle union_ = rectangle_mbr(r1, r2);
    union_ = rectangle_mbr(&union_, r3);
    return rectangle_length(&union_) - cover_area;
}

static void insert_foothold(struct FootholdRTree *tree, struct Foothold *fh)
{
    struct Rectangle fh_rect = foothold_mbr(fh);
    if (tree->root == NULL) {
        tree->root = malloc(sizeof(struct RTreeNode));
        tree->root->parent = NULL;
        tree->root->bound = fh_rect;
        tree->root->isLeaf = true;
        tree->root->count = 1;
        tree->root->footholds[0] = *fh;
    } else {
        struct RTreeNode *node = tree->root;
        // ChooseLeaf
        while (!node->isLeaf) {
            struct Rectangle bounding = rectangle_mbr(&node->children[0]->bound, &fh_rect);
            int32_t min_measure = rectangle_area(&bounding) - rectangle_area(&node->children[0]->bound);
            bool min_is_length = rectangle_area(&bounding) == 0 && min_measure == 0;
            if (min_is_length)
                min_measure = rectangle_length(&bounding) - rectangle_length(&node->children[0]->bound);

            uint8_t min_index = 0;
            for (uint8_t i = 1; i < node->count; i++) {
                struct Rectangle bounding = rectangle_mbr(&node->children[i]->bound, &fh_rect);
                int32_t measure = rectangle_area(&bounding) - rectangle_area(&node->children[i]->bound);
                bool length = rectangle_area(&bounding) == 0 && measure == 0;
                if (length)
                    measure = rectangle_length(&bounding) - rectangle_length(&node->children[i]->bound);

                if (!min_is_length && !length) {
                    if (measure == min_measure) {
                        // Pick the smallest rectangle of the two
                        if (rectangle_area(&node->children[i]->bound) < rectangle_area(&node->children[min_index]->bound)) {
                            min_index = i;
                        }
                    } else if (measure < min_measure) {
                        min_measure = measure;
                        min_index = i;
                    }
                } else if (min_is_length && length) {
                    if (measure == min_measure) {
                        // Pick the smallest rectangle of the two
                        if (rectangle_length(&node->children[i]->bound) < rectangle_length(&node->children[min_index]->bound)) {
                            min_index = i;
                        }
                    } else if (measure < min_measure) {
                        min_measure = measure;
                        min_index = i;
                    }
                } else if (length) {
                    min_measure = measure;
                    min_index = i;
                    min_is_length = true;
                }
                // Else the current minimum measure represents an area while the minimum represents a length, so we keep the minimum
            }

            node = node->children[min_index];
        }

        // SplitNode (brute force since M is sufficiently small)
        if (node->count < 3) {
            node->footholds[node->count] = *fh;
            node->count++;
            node->bound = rectangle_mbr(&node->bound, &fh_rect);
            struct RTreeNode *parent = node->parent;
            while (parent != NULL) {
                parent->bound = rectangle_mbr(&parent->bound, &node->bound);
                node = parent;
                parent = parent->parent;
            }
        } else {
            struct Rectangle fh0_rect = foothold_mbr(&node->footholds[0]);
            struct Rectangle fh1_rect = foothold_mbr(&node->footholds[1]);
            struct Rectangle fh2_rect = foothold_mbr(&node->footholds[2]);
            struct Rectangle child_rects[3] = { fh0_rect, fh1_rect, fh2_rect };
            int32_t min_measure = white_space_area_3(&fh1_rect, &fh2_rect, &fh_rect);
            bool min_is_length = rectangle_area(&fh1_rect) == 0 && rectangle_area(&fh2_rect) == 0 && min_measure == 0;
            if (min_is_length)
                min_measure = white_space_length_3(&fh1_rect, &fh2_rect, &fh_rect);

            bool min_is_one = true;
            uint8_t which[2] = { 0 };
            for (uint8_t i = 1; i < 4; i++) {
                struct Rectangle *r1 = &fh0_rect;
                struct Rectangle *r2 = i == 1 ? &fh2_rect : &fh1_rect;
                struct Rectangle *r3 = i == 2 ? &fh_rect : &fh2_rect;
                int32_t measure = white_space_area_3(r1, r2, r3);
                bool is_length = rectangle_area(r1) == 0 && rectangle_area(r2) == 0 && measure == 0;
                if (is_length)
                    measure = white_space_length_3(r1, r2, r3);

                if (((!min_is_length && !is_length) || (min_is_length && is_length)) && measure < min_measure) {
                    min_measure = measure;
                    which[0] = i;
                } else if (is_length) {
                    min_measure = measure;
                    which[0] = i;
                    min_is_length = true;
                }
            }

            for (uint8_t i = 0; i < 3; i++) {
                for (uint8_t j = i + 1; j < 4; j++) {
                    struct Rectangle *r1 = &child_rects[i];
                    struct Rectangle *r2 = j == 3 ? &fh_rect : &child_rects[i];
                    struct Rectangle *r3, *r4;
                    uint8_t k;
                    for (k = 0; k < 3; k++) {
                        if (k != i && k != j) {
                            r3 = &child_rects[k];
                            break;
                        }
                    }

                    for (uint8_t l = k + 1; l < 4; l++) {
                        if (l != i && l != j) {
                            r4 = l == 3 ? &fh_rect : &child_rects[l];
                            break;
                        }
                    }

                    int32_t measure1 = white_space_area_2(r1, r2);
                    int32_t measure2 = white_space_area_2(r3, r4);
                    bool is_length = rectangle_area(r1) == 0 && measure1 == 0 && rectangle_area(r3) && measure2 == 0;
                    if (is_length) {
                        measure1 = white_space_length_2(r1, r2);
                        measure2 = white_space_length_2(r3, r4);
                    }
                    if (((!min_is_length && !is_length) || (min_is_length && is_length)) && measure1 + measure2 < min_measure) {
                        min_measure = measure1 + measure2;
                        min_is_one = false;
                        which[0] = i;
                        which[1] = j;
                    } else if (is_length) {
                        min_measure = measure1 + measure2;
                        min_is_one = false;
                        which[0] = i;
                        which[1] = j;
                        min_is_length = true;
                    }
                }
            }

            struct RTreeNode *new = malloc(sizeof(struct RTreeNode));
            new->isLeaf = true;

            if (min_is_one) {
                new->count = 1;
                new->footholds[0] = which[0] == 3 ? *fh : node->footholds[which[0]];

                uint8_t j = 0;
                for (uint32_t i = 0; i < 4; i++) {
                    if (i != which[0]) {
                        node->footholds[j] = i == 3 ? *fh : node->footholds[i];
                        j++;
                    }
                }
            } else {
                new->count = 2;
                uint8_t i;
                for (i = 0; i < 3; i++) {
                    if (i != which[0] && i != which[1]) {
                        new->footholds[0] = node->footholds[i];
                        break;
                    }
                }

                for (uint8_t j = i + 1; j < 4; j++) {
                    if (j != which[0] && j != which[1]) {
                        new->footholds[1] = j == 3 ? *fh : node->footholds[j];
                        break;
                    }
                }

                node->count = 2;
                node->footholds[0] = node->footholds[which[0]];
                node->footholds[1] = which[1] == 3 ? *fh : node->footholds[which[1]];
            }

            fh0_rect = foothold_mbr(&node->footholds[0]);
            fh1_rect = foothold_mbr(&node->footholds[1]);
            fh2_rect = foothold_mbr(&node->footholds[2]);
            struct Rectangle bound = rectangle_mbr(&fh0_rect, &fh1_rect);
            node->bound = node->count == 2 ? bound : rectangle_mbr(&bound, &fh2_rect);

            struct Rectangle new_fh0_rect = foothold_mbr(&new->footholds[0]);
            struct Rectangle new_fh1_rect = foothold_mbr(&new->footholds[1]);
            new->bound = new->count == 2 ? rectangle_mbr(&new_fh0_rect, &new_fh1_rect) : new_fh0_rect;

            struct RTreeNode *parent = node->parent;
            while (parent != NULL) {
                if (parent->count < 3) {
                    node = parent;
                    node->children[node->count] = new;
                    new->parent = node;
                    node->count++;
                    while (node != NULL) {
                        struct Rectangle bound = node->children[0]->bound;
                        for (uint8_t i = 1; i < node->count; i++) {
                            bound = rectangle_mbr(&bound, &node->children[i]->bound);
                        }
                        node->bound = bound;
                        node = node->parent;
                    }
                    break;
                } else {
                    node = parent;

                    int32_t min_measure = white_space_area_3(&node->children[1]->bound, &node->children[2]->bound, &new->bound);
                    bool min_is_length = rectangle_area(&node->children[1]->bound) == 0 && rectangle_area(&node->children[2]->bound) == 0 && min_measure == 0;
                    if (min_is_length)
                        min_measure = white_space_length_3(&node->children[1]->bound, &node->children[2]->bound, &new->bound);

                    bool min_is_one = true;
                    uint8_t which[2] = { 0 };
                    for (uint8_t i = 1; i < 4; i++) {
                        struct Rectangle *r1 = &node->children[0]->bound;
                        struct Rectangle *r2 = i == 1 ? &node->children[2]->bound : &node->children[1]->bound;
                        struct Rectangle *r3 = i == 2 ? &new->bound : &node->children[2]->bound;
                        int32_t measure = white_space_area_3(r1, r2, r3);
                        bool is_length = rectangle_area(r1) == 0 && rectangle_area(r2) == 0 && measure == 0;
                        if (is_length)
                            measure = white_space_length_3(r1, r2, r3);

                        if (((!min_is_length && !is_length) || (min_is_length && is_length)) && measure < min_measure) {
                            min_measure = measure;
                            which[0] = i;
                        } else if (is_length) {
                            min_measure = measure;
                            which[0] = i;
                            min_is_length = true;
                        }
                    }

                    for (uint8_t i = 0; i < 3; i++) {
                        for (uint8_t j = i + 1; j < 4; j++) {
                            struct Rectangle *r1 = &node->children[i]->bound;
                            struct Rectangle *r2 = j == 3 ? &new->bound : &node->children[j]->bound;
                            struct Rectangle *r3, *r4;
                            uint8_t k;
                            for (k = 0; k < 3; k++) {
                                if (k != i && k != j) {
                                    r3 = &node->children[k]->bound;
                                    break;
                                }
                            }

                            for (uint8_t l = k + 1; l < 4; l++) {
                                if (l != i && l != j) {
                                    r4 = l == 3 ? &new->bound : &node->children[l]->bound;
                                    break;
                                }
                            }

                            int32_t measure1 = white_space_area_2(r1, r2);
                            int32_t measure2 = white_space_area_2(r3, r4);
                            bool is_length = rectangle_area(r1) == 0 && measure1 == 0 && rectangle_area(r3) && measure2 == 0;
                            if (is_length) {
                                measure1 = white_space_length_2(r1, r2);
                                measure2 = white_space_length_2(r3, r4);
                            }
                            if (((!min_is_length && !is_length) || (min_is_length && is_length)) && measure1 + measure2 < min_measure) {
                                min_measure = measure1 + measure2;
                                min_is_one = false;
                                which[0] = i;
                                which[1] = j;
                            } else if (is_length) {
                                min_measure = measure1 + measure2;
                                min_is_one = false;
                                which[0] = i;
                                which[1] = j;
                                min_is_length = true;
                            }
                        }
                    }

                    struct RTreeNode *new_node = malloc(sizeof(struct RTreeNode));
                    new_node->isLeaf = false;

                    if (min_is_one) {
                        new_node->count = 1;
                        new_node->children[0] = which[0] == 3 ? new : node->children[which[0]];
                        if (which[0] == 3)
                            new->parent = new_node;
                        else
                            node->children[which[0]]->parent = new_node;

                        uint8_t j = 0;
                        for (uint32_t i = 0; i < 4; i++) {
                            if (i != which[0]) {
                                node->children[j] = i == 3 ? new : node->children[i];
                                if (i == 3)
                                    new->parent = node;
                                j++;
                            }
                        }

                    } else {
                        new_node->count = 2;
                        uint8_t i;
                        for (i = 0; i < 3; i++) {
                            if (i != which[0] && i != which[1]) {
                                new_node->children[0] = node->children[i];
                                node->children[i]->parent = new_node;
                                break;
                            }
                        }

                        for (uint8_t j = i + 1; j < 4; j++) {
                            if (j != which[0] && j != which[1]) {
                                new_node->children[1] = j == 3 ? new : node->children[j];
                                if (j == 3)
                                    new->parent = new_node;
                                else
                                    node->children[j]->parent = new_node;
                                break;
                            }
                        }

                        node->count = 2;
                        node->children[0] = node->children[which[0]];
                        node->children[1] = which[1] == 3 ? new : node->children[which[1]];
                        if (which[1] == 3)
                            new->parent = node;
                    }

                    new = new_node;
                    struct Rectangle bound = rectangle_mbr(&node->children[0]->bound, &node->children[1]->bound);
                    node->bound = node->count == 2 ? bound : rectangle_mbr(&bound, &node->children[2]->bound);
                    new->bound = new->count == 2 ? rectangle_mbr(&new->children[0]->bound, &new->children[1]->bound) : new->children[0]->bound;
                    parent = node->parent;
                }
            }

            if (parent == NULL) {
                tree->root = malloc(sizeof(struct RTreeNode));
                tree->root->parent = NULL;
                tree->root->bound = rectangle_mbr(&node->bound, &new->bound);
                tree->root->isLeaf = false;
                tree->root->count = 2;
                tree->root->children[0] = node;
                node->parent = tree->root;
                tree->root->children[1] = new;
                new->parent = tree->root;
            }
        }
    }
}


#include "client.h"

#include <assert.h>
#include <threads.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include "../constants.h"
#include "../hash-map.h"
#include "../packet.h"
#include "../party.h"
#include "map.h"
#include "scripting/client.h"
#include "shop.h"

struct Client {
    struct Session *session;
    struct MapHandleContainer map;
    struct DatabaseConnection *conn;
    struct ClientSave *save;
    struct {
        struct ScriptManager *quest;
        struct ScriptManager *npc;
        struct ScriptManager *portal;
        struct ScriptManager *map;
    } managers;
    enum PacketType handlerType;
    struct Character character;
    struct ScriptInstance *script;

    // Values related to the current script state
    struct {
        enum ScriptState scriptState;
        uint32_t min;
        uint32_t max;
    };

    uint16_t qid;
    uint32_t npc;
    uint32_t shop;
    struct HashSetU32 *visibleMapObjects;

    struct DatabaseRequest *request;
    int databaseState;
    enum Stat stats;
    struct Party *party;
    bool autoPickup;
};

struct ClientSave {
    int state;
    struct DatabaseConnection *conn;
    struct DatabaseRequest *request;
    struct RequestParams *params;
    struct UserEvent *event;
};

size_t CLIENTS_LENGTH;
struct {
    uint8_t len;
    char name[CHARACTER_MAX_NAME_LENGTH];
    uint32_t id;
} *CLIENTS;

mtx_t CLIENTS_LOCK;

static void insert_client(uint8_t len, const char *name, uint32_t id);
static uint32_t find_id_by_name(uint8_t len, const char *name);

static bool check_quest_requirements(struct Character *chr, size_t req_count, const struct QuestRequirement *reqs, uint32_t npc);
static bool start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success);
static bool end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success);

struct AddQuestContext {
    uint16_t *quests;
    size_t currentQuest;
    size_t progressCount;
};

static void add_quest(void *data, void *ctx);

struct AddProgressContext {
    struct DatabaseProgress *progresses;
    size_t currentProgress;
};

static void add_progress(void *data, void *ctx);

struct AddQuestInfoContext {
    struct DatabaseInfoProgress *infos;
    size_t currentInfo;
};

static void add_quest_info(void *data, void *ctx);

struct AddCompletedQuestContext {
    struct DatabaseCompletedQuest *quests;
    size_t currentQuest;
};

static void add_completed_quest(void *data, void *ctx);

struct AddSkillContext {
    struct DatabaseSkill *skills;
    size_t currentSkill;
};

static void add_skill(void *data, void *ctx);

struct AddMonsterBookContext {
    struct DatabaseMonsterBookEntry *monsterBook;
    size_t currentEntry;
};

static void add_monster_book_entry(void *data, void *ctx);

enum ClientCommandType {
    CLIENT_COMMAND_PARTY_INVITE,
    CLIENT_COMMAND_PARTY_REJECT,
    CLIENT_COMMAND_PARTY_JOIN,
    CLIENT_COMMAND_PARTY_LEAVE,
    CLIENT_COMMAND_PARTY_KICK,
    CLIENT_COMMAND_PARTY_DISBAND,
    CLIENT_COMMAND_PARTY_LOG_ON_OFF,
    CLIENT_COMMAND_PARTY_CHANGE_LEADER,
};

struct ClientCommand {
    enum ClientCommandType type;
    uint8_t reason; // Rejection reason; 0 - player initiated; 1 - already in party
    uint32_t id;
    uint8_t nameLen;
    char name[CHARACTER_MAX_NAME_LENGTH];
};

int clients_init(void)
{
    return mtx_init(&CLIENTS_LOCK, mtx_plain);
}

void clients_terminate(void)
{
    free(CLIENTS);
    mtx_destroy(&CLIENTS_LOCK);
}

struct Client *client_create(struct Session *session, struct DatabaseConnection *conn, struct ScriptManager *quest_manager, struct ScriptManager *portal_mananger, struct ScriptManager *npc_manager, struct ScriptManager *map_manager)
{
    struct Client *client = malloc(sizeof(struct Client));
    if (client == NULL)
        return NULL;

    client->visibleMapObjects = hash_set_u32_create(sizeof(uint32_t), 0);
    if (client->visibleMapObjects == NULL) {
        free(client);
        return NULL;
    }

    client->session = session;
    client->conn = conn;
    client->map.player = NULL;
    client->managers.quest = quest_manager;
    client->managers.portal = portal_mananger;
    client->managers.npc = npc_manager;
    client->managers.map = map_manager;
    client->character.quests = NULL;
    client->character.monsterQuests = NULL;
    client->character.itemQuests = NULL;
    client->character.questInfos = NULL;
    client->character.completedQuests = NULL;
    client->character.skills = NULL;
    client->character.monsterBook = NULL;
    client->script = NULL;
    client->shop = -1;
    client->stats = 0;
    client->party = NULL;
    client->autoPickup = false;
    client->handlerType = PACKET_TYPE_NONE;

    return client;
}

void client_destroy(struct Client *client)
{
    hash_set_u32_destroy(client->character.monsterBook);
    hash_set_u32_destroy(client->character.skills);
    hash_set_u16_destroy(client->character.completedQuests);
    hash_set_u16_destroy(client->character.questInfos);
    hash_set_u32_destroy(client->character.itemQuests);
    hash_set_u32_destroy(client->character.monsterQuests);
    hash_set_u16_destroy(client->character.quests);
    hash_set_u32_destroy(client->visibleMapObjects);
    free(client);
}

struct Session *client_get_session(struct Client *client)
{
    return client->session;
}

void client_login_start(struct Client *client, uint32_t id)
{
    client->handlerType = PACKET_TYPE_LOGIN;
    client->databaseState = 0;
    client->character.id = id;
}

static void on_resume_database_operation(void *data, int fd, int status)
{
    struct ClientSave *save = data;

    status = database_request_execute(save->request, status);
    if (status <= 0) {
        const struct RequestParams *params = database_request_get_params(save->request);
        free(params->updateCharacter.keyMap);
        free(params->updateCharacter.monsterBook);
        free(params->updateCharacter.skills);
        free(params->updateCharacter.completedQuests);
        free(params->updateCharacter.questInfos);
        free(params->updateCharacter.progresses);
        free(params->updateCharacter.quests);
        database_request_destroy(save->request);
        database_connection_unlock(save->conn);
        free(save->params);
        free(save);
        return;
    }

    save->event = user_event_add_event(save->event, status, fd, on_resume_database_operation, save);
}

static void start_save(struct Client *client)
{
    struct Character *chr = &client->character;
    const struct RequestParams *params = database_request_get_params(client->request);
    const union DatabaseResult *res = database_request_result(client->request);
    size_t count = 0;

    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < chr->inventory[i].slotCount; j++) {
            if (!chr->inventory[i].items[j].isEmpty && chr->inventory[i].items[j].item.item.id == 0) {
                chr->inventory[i].items[j].item.item.id = res->allocateIds.items[count];
                count++;
            }
        }
    }

    count = 0;
    for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (!chr->equippedEquipment[i].isEmpty && chr->equippedEquipment[i].equip.id == 0) {
            chr->equippedEquipment[i].equip.item.id = params->allocateIds.equippedEquipment[count].id;
            chr->equippedEquipment[i].equip.equipId = params->allocateIds.equippedEquipment[count].equipId;
            chr->equippedEquipment[i].equip.id = res->allocateIds.equippedEquipment[count];
            count++;
        }
    }

    count = 0;
    for (size_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        if (!chr->equipmentInventory.items[i].isEmpty && chr->equipmentInventory.items[i].equip.id == 0) {
            chr->equipmentInventory.items[i].equip.item.id = params->allocateIds.equipmentInventory[count].id;
            chr->equipmentInventory.items[i].equip.equipId = params->allocateIds.equipmentInventory[count].equipId;
            chr->equipmentInventory.items[i].equip.id = res->allocateIds.equipmentInventory[count];
            count++;
        }
    }

    database_request_destroy(client->request);

    struct ClientSave *save = malloc(sizeof(struct ClientSave));
    if (save == NULL) {
        database_connection_unlock(client->conn);
        session_kick(client->session);
        return;
    }

    client->save = save;

    save->conn = client->conn;
    save->state = 0;
    save->params = malloc(sizeof(struct RequestParams));
    if (save->params == NULL) {
        database_connection_unlock(client->conn);
        free(save);
        session_kick(client->session);
        return;
    }

    save->params->type = DATABASE_REQUEST_TYPE_UPDATE_CHARACTER;
    save->params->updateCharacter.id = chr->id;
    save->params->updateCharacter.accountId = chr->accountId;
    save->params->updateCharacter.map = wz_get_map_forced_return(chr->map);
    save->params->updateCharacter.spawnPoint = chr->spawnPoint;
    save->params->updateCharacter.job = chr->job;
    save->params->updateCharacter.level = chr->level;
    save->params->updateCharacter.exp = chr->exp;
    save->params->updateCharacter.maxHp = chr->maxHp;
    save->params->updateCharacter.hp = chr->hp;
    save->params->updateCharacter.maxMp = chr->maxMp;
    save->params->updateCharacter.mp = chr->mp;
    save->params->updateCharacter.str = chr->str;
    save->params->updateCharacter.dex = chr->dex;
    save->params->updateCharacter.int_ = chr->int_;
    save->params->updateCharacter.luk = chr->luk;
    save->params->updateCharacter.ap = chr->ap;
    save->params->updateCharacter.sp = chr->sp;
    save->params->updateCharacter.fame = chr->fame;
    save->params->updateCharacter.skin = chr->skin;
    save->params->updateCharacter.face = chr->face;
    save->params->updateCharacter.hair = chr->hair;
    save->params->updateCharacter.mesos = chr->mesos;
    save->params->updateCharacter.equipSlots = chr->equipmentInventory.slotCount;
    save->params->updateCharacter.useSlots = chr->inventory[0].slotCount;
    save->params->updateCharacter.setupSlots = chr->inventory[1].slotCount;
    save->params->updateCharacter.etcSlots = chr->inventory[2].slotCount;
    save->params->updateCharacter.equippedCount = 0;
    save->params->updateCharacter.equipCount = 0;
    save->params->updateCharacter.itemCount = 0;

    for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (!chr->equippedEquipment[i].isEmpty) {
            struct Equipment *src = &chr->equippedEquipment[i].equip;
            struct DatabaseCharacterEquipment *dst =
                &save->params->updateCharacter.equippedEquipment[save->params->updateCharacter.equippedCount];

            dst->id = src->id;
            dst->equip.id = src->equipId;
            dst->equip.item.id = src->item.id;
            dst->equip.item.itemId = src->item.itemId;
            dst->equip.item.flags = src->item.flags;
            dst->equip.item.ownerLength = src->item.ownerLength;
            memcpy(dst->equip.item.owner, src->item.owner,
                    src->item.ownerLength);
            dst->equip.item.giverLength = src->item.giftFromLength;
            memcpy(dst->equip.item.giver, src->item.giftFrom,
                    src->item.giftFromLength);


            dst->equip.level = src->level;
            dst->equip.slots = src->slots;
            dst->equip.str = src->str;
            dst->equip.dex = src->dex;
            dst->equip.int_ = src->int_;
            dst->equip.luk = src->luk;
            dst->equip.hp = src->hp;
            dst->equip.mp = src->mp;
            dst->equip.atk = src->atk;
            dst->equip.matk = src->matk;
            dst->equip.def = src->def;
            dst->equip.mdef = src->mdef;
            dst->equip.acc = src->acc;
            dst->equip.avoid = src->avoid;
            dst->equip.speed = src->speed;
            dst->equip.jump = src->jump;

            save->params->updateCharacter.equippedCount++;
        }
    }

    for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        if (!chr->equipmentInventory.items[i].isEmpty) {
            struct Equipment *src = &chr->equipmentInventory.items[i].equip;
            struct DatabaseCharacterEquipment *dst =
                &save->params->updateCharacter.equipmentInventory[save->params->updateCharacter.equipCount].equip;

            dst->id = src->id;
            dst->equip.id = src->equipId;
            save->params->updateCharacter.equipmentInventory[save->params->updateCharacter.equipCount].slot = i;
            dst->equip.item.id = src->item.id;
            dst->equip.item.itemId = src->item.itemId;
            dst->equip.item.flags = src->item.flags;
            dst->equip.item.ownerLength = src->item.ownerLength;
            memcpy(dst->equip.item.owner, src->item.owner, src->item.ownerLength);
            dst->equip.item.giverLength = src->item.giftFromLength;
            memcpy(dst->equip.item.giver, src->item.giftFrom, src->item.giftFromLength);

            dst->equip.level = src->level;
            dst->equip.slots = src->slots;
            dst->equip.str = src->str;
            dst->equip.dex = src->dex;
            dst->equip.int_ = src->int_;
            dst->equip.luk = src->luk;
            dst->equip.hp = src->hp;
            dst->equip.mp = src->mp;
            dst->equip.atk = src->atk;
            dst->equip.matk = src->matk;
            dst->equip.def = src->def;
            dst->equip.mdef = src->mdef;
            dst->equip.acc = src->acc;
            dst->equip.avoid = src->avoid;
            dst->equip.speed = src->speed;
            dst->equip.jump = src->jump;

            save->params->updateCharacter.equipCount++;
        }
    }

    for (uint8_t inv = 0; inv < 4; inv++) {
        for (uint8_t i = 0; i < chr->inventory[inv].slotCount; i++) {
            if (!chr->inventory[inv].items[i].isEmpty) {
                struct Item *src = &chr->inventory[inv].items[i].item.item;
                struct DatabaseItem *dst =
                    &save->params->updateCharacter.inventoryItems[save->params->updateCharacter.itemCount].item;
                save->params->updateCharacter.inventoryItems[save->params->updateCharacter.itemCount].slot = i;
                save->params->updateCharacter.inventoryItems[save->params->updateCharacter.itemCount].count =
                    chr->inventory[inv].items[i].item.quantity;

                dst->id = src->id;
                dst->itemId = src->itemId;
                dst->flags = src->flags;
                dst->ownerLength = src->ownerLength;
                memcpy(dst->owner, src->owner, src->ownerLength);
                dst->giverLength = src->giftFromLength;
                memcpy(dst->giver, src->giftFrom, src->giftFromLength);

                save->params->updateCharacter.itemCount++;
            }
        }
    }

    save->params->updateCharacter.quests = malloc(hash_set_u16_size(chr->quests) * sizeof(uint16_t));
    if (save->params->updateCharacter.quests == NULL) {
        database_connection_unlock(client->conn);
        free(save->params);
        free(save);
        session_kick(client->session);
        return;
    }

    struct AddQuestContext ctx = {
        .quests = save->params->updateCharacter.quests,
        .currentQuest = 0,
        .progressCount = 0
    };
    hash_set_u16_foreach(chr->quests, add_quest, &ctx);

    save->params->updateCharacter.questCount = ctx.currentQuest;

    save->params->updateCharacter.progresses = malloc(ctx.progressCount * sizeof(struct DatabaseProgress));
    if (save->params->updateCharacter.progresses == NULL) {
        database_connection_unlock(client->conn);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        session_kick(client->session);
    }

    struct AddProgressContext ctx2 = {
        .progresses = save->params->updateCharacter.progresses,
        .currentProgress = 0
    };
    hash_set_u16_foreach(chr->quests, add_progress, &ctx2);

    save->params->updateCharacter.progressCount = ctx2.currentProgress;

    save->params->updateCharacter.questInfos = malloc(hash_set_u16_size(chr->questInfos) * sizeof(struct DatabaseInfoProgress));
    if (save->params->updateCharacter.questInfos == NULL) {
        database_connection_unlock(client->conn);
        free(save->params->updateCharacter.progresses);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        session_kick(client->session);
    }

    struct AddQuestInfoContext ctx3 = {
        .infos = save->params->updateCharacter.questInfos,
        .currentInfo = 0,
    };
    hash_set_u16_foreach(chr->questInfos, add_quest_info, &ctx3);

    save->params->updateCharacter.questInfoCount = ctx3.currentInfo;

    save->params->updateCharacter.completedQuests = malloc(hash_set_u16_size(chr->completedQuests) * sizeof(struct DatabaseCompletedQuest));
    if (save->params->updateCharacter.completedQuests == NULL) {
        database_connection_unlock(client->conn);
        free(save->params->updateCharacter.questInfos);
        free(save->params->updateCharacter.progresses);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        session_kick(client->session);
    }

    struct AddCompletedQuestContext ctx4 = {
        .quests = save->params->updateCharacter.completedQuests,
        .currentQuest = 0,
    };
    hash_set_u16_foreach(chr->completedQuests, add_completed_quest, &ctx4);

    save->params->updateCharacter.completedQuestCount = ctx4.currentQuest;

    save->params->updateCharacter.skills = malloc(hash_set_u32_size(chr->skills) * sizeof(struct DatabaseSkill));
    if (save->params->updateCharacter.completedQuests == NULL) {
        database_connection_unlock(client->conn);
        free(save->params->updateCharacter.completedQuests);
        free(save->params->updateCharacter.questInfos);
        free(save->params->updateCharacter.progresses);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        session_kick(client->session);
    }

    struct AddSkillContext ctx5 = {
        .skills = save->params->updateCharacter.skills,
        .currentSkill = 0,
    };
    hash_set_u32_foreach(chr->skills, add_skill, &ctx5);

    save->params->updateCharacter.skillCount = ctx5.currentSkill;

    save->params->updateCharacter.monsterBook = malloc(hash_set_u32_size(chr->monsterBook) * sizeof(struct DatabaseMonsterBookEntry));
    if (save->params->updateCharacter.monsterBook == NULL) {
        database_connection_unlock(client->conn);
        free(save->params->updateCharacter.skills);
        free(save->params->updateCharacter.completedQuests);
        free(save->params->updateCharacter.questInfos);
        free(save->params->updateCharacter.progresses);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        session_kick(client->session);
    }

    struct AddMonsterBookContext ctx6 = {
        .monsterBook = save->params->updateCharacter.monsterBook,
        .currentEntry = 0,
    };
    hash_set_u32_foreach(chr->monsterBook, add_monster_book_entry, &ctx6);

    save->params->updateCharacter.monsterBookEntryCount = ctx6.currentEntry;

    uint8_t key_count = 0;
    for (uint8_t i = 0; i < KEYMAP_MAX_KEYS; i++) {
        if (chr->keyMap[i].type != 0)
            key_count++;
    }

    save->params->updateCharacter.keyMap = malloc(key_count * sizeof(struct DatabaseKeyMapEntry));
    if (save->params->updateCharacter.keyMap == NULL) {
        database_connection_unlock(client->conn);
        free(save->params->updateCharacter.monsterBook);
        free(save->params->updateCharacter.skills);
        free(save->params->updateCharacter.completedQuests);
        free(save->params->updateCharacter.questInfos);
        free(save->params->updateCharacter.progresses);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        session_kick(client->session);
    }

    save->params->updateCharacter.keyMapEntryCount = 0;
    for (uint8_t i = 0; i < KEYMAP_MAX_KEYS; i++) {
        if (chr->keyMap[i].type != 0) {
            save->params->updateCharacter.keyMap[save->params->updateCharacter.keyMapEntryCount].key = i;
            save->params->updateCharacter.keyMap[save->params->updateCharacter.keyMapEntryCount].type = chr->keyMap[i].type;
            save->params->updateCharacter.keyMap[save->params->updateCharacter.keyMapEntryCount].action = chr->keyMap[i].action;
            save->params->updateCharacter.keyMapEntryCount++;
        }
    }

    save->request = database_request_create(save->conn, save->params);
    if (save->request == NULL) {
        database_connection_unlock(client->conn);
        free(save->params->updateCharacter.keyMap);
        free(save->params->updateCharacter.monsterBook);
        free(save->params->updateCharacter.skills);
        free(save->params->updateCharacter.completedQuests);
        free(save->params->updateCharacter.questInfos);
        free(save->params->updateCharacter.progresses);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        session_kick(client->session);
    }

    int status = database_request_execute(save->request, 0);
    if (status <= 0) {
        database_request_destroy(save->request);
        database_connection_unlock(save->conn);
        free(save->params->updateCharacter.keyMap);
        free(save->params->updateCharacter.monsterBook);
        free(save->params->updateCharacter.skills);
        free(save->params->updateCharacter.completedQuests);
        free(save->params->updateCharacter.questInfos);
        free(save->params->updateCharacter.progresses);
        free(save->params->updateCharacter.quests);
        free(save->params);
        free(save);
        if (status < 0)
            session_kick(client->session);
        return;
    }

    save->event = session_add_event(client->session, status, database_connection_get_fd(save->conn), on_resume_database_operation, save);
}

static void on_resume_id_allocate_database_operation(struct Session *session, int fd, int status)
{
    struct Client *client = session_get_context(session);

    status = database_request_execute(client->request, status);
    if (status != 0) {
        if (status > 0) {
            session_set_event(session, status, fd, on_resume_id_allocate_database_operation);
        } else if (status < 0) {
            database_request_destroy(client->request);
            database_connection_unlock(client->conn);
            session_close_event(session);
            session_kick(session);
        }
        return;
    } else {
        session_close_event(session);
    }

    start_save(client);
}

static void on_id_allocate_database_unlocked(struct Session *session, int fd, int status)
{
    struct Client *client = session_get_context(session);
    struct Character *chr = &client->character;
    assert(status == POLLIN);
    close(fd);

    struct RequestParams params = {
        .type = DATABASE_REQUEST_TYPE_ALLOCATE_IDS,
        .allocateIds = {
            .id = chr->id,
            .accountId = chr->accountId,
            .itemCount = 0,
            .equippedCount = 0,
            .equipCount = 0,
            .storageItemCount = 0,
            .storageEquipCount = 0
        },
    };

    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < chr->inventory[i].slotCount; j++) {
            if (!chr->inventory[i].items[j].isEmpty && chr->inventory[i].items[j].item.item.id == 0) {
                params.allocateIds.items[params.allocateIds.itemCount] = chr->inventory[i].items[j].item.item.itemId;
                params.allocateIds.itemCount++;
            }
        }
    }

    for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (!chr->equippedEquipment[i].isEmpty && chr->equippedEquipment[i].equip.id == 0) {
            params.allocateIds.equippedEquipment[params.allocateIds.equippedCount].id =
                chr->equippedEquipment[i].equip.item.id;
            params.allocateIds.equippedEquipment[params.allocateIds.equippedCount].equipId =
                chr->equippedEquipment[i].equip.equipId;
            params.allocateIds.equippedEquipment[params.allocateIds.equippedCount].itemId =
                chr->equippedEquipment[i].equip.item.itemId;
            params.allocateIds.equippedCount++;
        }
    }

    for (size_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        if (!chr->equipmentInventory.items[i].isEmpty && chr->equipmentInventory.items[i].equip.id == 0) {
            params.allocateIds.equipmentInventory[params.allocateIds.equipCount].id =
                chr->equipmentInventory.items[i].equip.item.id;
            params.allocateIds.equipmentInventory[params.allocateIds.equipCount].equipId =
                chr->equipmentInventory.items[i].equip.equipId;
            params.allocateIds.equipmentInventory[params.allocateIds.equipCount].itemId =
                chr->equipmentInventory.items[i].equip.item.itemId;
            params.allocateIds.equipCount++;
        }
    }

    client->request = database_request_create(client->conn, &params);
    if (client->request == NULL) {
        database_connection_unlock(client->conn);
        return;
    }

    status = database_request_execute(client->request, 0);
    if (status != 0) {
        if (status > 0) {
            session_set_event(session, status, database_connection_get_fd(client->conn), on_resume_id_allocate_database_operation);
        } else if (status < 0) {
            database_request_destroy(client->request);
            database_connection_unlock(client->conn);
            session_close_event(session);
            session_kick(session);
        }
        return;
    } else {
        session_close_event(session);
    }

    start_save(client);
}

int client_save_start(struct Client *client)
{
    int fd = database_connection_lock(client->conn);
    if (fd == -2) {
        on_id_allocate_database_unlocked(client->session, fd, POLLIN);
    } else if (fd == -1) {
        return -1;
    } else {
        return session_set_event(client->session, POLLIN, fd, on_id_allocate_database_unlocked);
    }

    return 0;
}

void client_logout_start(struct Client *client)
{
    script_manager_free(client->script);
    if (client->map.player != NULL)
        map_leave(room_get_context(session_get_room(client->session)), client->map.player);
    client->handlerType = PACKET_TYPE_LOGOUT;
    client->databaseState = 0;
}

struct ClientContResult client_resume(struct Client *client, int status)
{
    struct Character *chr = &client->character;

    switch (client->handlerType) {
    case PACKET_TYPE_LOGIN:
        if (client->databaseState == 0) {
            int fd = database_connection_lock(client->conn);
            if (fd == -2) {
                client->databaseState++;
            } else if (fd == -1) {
                return (struct ClientContResult) { .status = -1 };
            } else {
                client->databaseState++;
                return (struct ClientContResult) { .status = POLLIN, .fd = fd };
            }
        }

        if (client->databaseState == 1) {
            if (status != 0)
                close(session_close_event(client->session));

            struct RequestParams params = {
                .type = DATABASE_REQUEST_TYPE_GET_CHARACTER,
                .getCharacter = {
                    .id = client->character.id
                }
            };

            client->request = database_request_create(client->conn, &params);
            client->databaseState++;
            status = database_request_execute(client->request, 0);
            if (status < 0) {
                database_request_destroy(client->request);
                return (struct ClientContResult) { -1 };
            } else if (status > 0) {
                return (struct ClientContResult) { status, database_connection_get_fd(client->conn) };
            }
            client->databaseState++;
        }

        if (client->databaseState == 2) {
            status = database_request_execute(client->request, status);
            if (status < 0) {
                database_request_destroy(client->request);
                return (struct ClientContResult) { -1 };
            } else if (status > 0) {
                return (struct ClientContResult) { status, database_connection_get_fd(client->conn) };
            }
            client->databaseState++;
        }

        if (client->databaseState == 3) {
            client->databaseState++;
            const union DatabaseResult *res = database_request_result(client->request);
            chr->nameLength = res->getCharacter.nameLength;
            memcpy(chr->name, res->getCharacter.name, res->getCharacter.nameLength);
            chr->map = res->getCharacter.map;
            // Will be updated in on_client_join()
            //chr->x = info->x;
            //chr->y = info->y;
            //chr->fh = 0;
            //chr->stance = 6;
            chr->chair = 0;
            chr->seat = -1;
            chr->spawnPoint = wz_get_portal_info_by_name(chr->map, "sp")->id;
            chr->level = res->getCharacter.level;
            chr->job = res->getCharacter.job;
            chr->exp = res->getCharacter.exp;
            chr->maxHp = res->getCharacter.maxHp;
            chr->eMaxHp = 0;
            chr->hp = res->getCharacter.hp;
            chr->maxMp = res->getCharacter.maxMp;
            chr->eMaxMp = 0;
            chr->mp = res->getCharacter.mp;
            chr->str = res->getCharacter.str;
            chr->estr = 0;
            chr->dex = res->getCharacter.dex;
            chr->edex = 0;
            chr->int_ = res->getCharacter.int_;
            chr->eint = 0;
            chr->luk = res->getCharacter.luk;
            chr->eluk = 0;
            chr->hpmp = res->getCharacter.hpmp;
            chr->ap = res->getCharacter.ap;
            chr->sp = res->getCharacter.sp;
            chr->fame = res->getCharacter.fame;
            chr->gender = res->getCharacter.gender == 0 ? ACCOUNT_GENDER_MALE : ACCOUNT_GENDER_FEMALE;
            chr->skin = res->getCharacter.skin;
            chr->face = res->getCharacter.face;
            chr->hair = res->getCharacter.hair;
            chr->mesos = res->getCharacter.mesos;
            chr->gachaExp = 0;
            chr->equipmentInventory.slotCount = res->getCharacter.equipSlots;
            chr->inventory[0].slotCount = res->getCharacter.useSlots;
            chr->inventory[1].slotCount = res->getCharacter.setupSlots;
            chr->inventory[2].slotCount = res->getCharacter.etcSlots;
            chr->inventory[3].slotCount = 252;

            for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++)
                chr->equippedEquipment[i].isEmpty = true;

            for (uint8_t i = 0; i < res->getCharacter.equippedCount; i++) {
                const struct DatabaseCharacterEquipment *src = &res->getCharacter.equippedEquipment[i];
                struct Equipment *dst = &chr->equippedEquipment[equip_slot_to_compact(equip_slot_from_id(res->getCharacter.equippedEquipment[i].equip.item.itemId))].equip;
                chr->equippedEquipment[equip_slot_to_compact(equip_slot_from_id(res->getCharacter.equippedEquipment[i].equip.item.itemId))].isEmpty = false;
                dst->id = src->id;
                dst->equipId = src->equip.id;
                dst->item.id = src->equip.item.id;
                dst->item.itemId = src->equip.item.itemId;
                //equip->item.cashId;
                //equip->item.sn; // What is this?

                dst->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
                                             //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
                dst->item.flags = src->equip.item.flags;
                //dst->item.expiration = src->expiration;
                dst->item.expiration = -1;
                dst->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
                                                //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
                dst->level = src->equip.level;
                dst->slots = src->equip.slots;
                dst->str = src->equip.str;
                chr->estr += dst->str;
                dst->dex = src->equip.dex;
                chr->edex += dst->dex;
                dst->int_ = src->equip.int_;
                chr->eint += dst->int_;
                dst->luk = src->equip.luk;
                chr->eluk += dst->luk;
                dst->hp = src->equip.hp;
                chr->eMaxHp += dst->hp;
                dst->mp = src->equip.mp;
                chr->eMaxMp += dst->mp;
                dst->atk = src->equip.atk;
                dst->matk = src->equip.matk;
                dst->def = src->equip.def;
                dst->mdef = src->equip.mdef;
                dst->acc = src->equip.acc;
                dst->avoid = src->equip.avoid;
                dst->hands = 0; //equip->hands = res->getCharacter.equippedEquipment[i].hands;
                dst->speed = src->equip.speed;
                dst->jump = src->equip.jump;
                dst->cash = wz_get_equip_info(dst->item.itemId)->cash;
            }

            for (uint8_t j = 0; j < chr->equipmentInventory.slotCount; j++)
                chr->equipmentInventory.items[j].isEmpty = true;

            for (uint8_t i = 0; i < res->getCharacter.equipCount; i++) {
                const struct DatabaseCharacterEquipment *src = &res->getCharacter.equipmentInventory[i].equip;
                struct Equipment *dst = &chr->equipmentInventory.items[res->getCharacter.equipmentInventory[i].slot].equip;
                chr->equipmentInventory.items[res->getCharacter.equipmentInventory[i].slot].isEmpty = false;
                dst->id = src->id;
                dst->equipId = src->equip.id;
                dst->item.id = src->equip.item.id;
                dst->item.itemId = src->equip.item.itemId;
                //equip->item.cashId;
                //equip->item.sn; // What is this?

                dst->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
                                             //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
                dst->item.flags = src->equip.item.flags;
                dst->item.expiration = -1; //equip->item.expiration = res->getCharacter.equippedEquipment[i].expiration;
                dst->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
                                                //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
                dst->level = src->equip.level;
                dst->slots = src->equip.slots;
                dst->str = src->equip.str;
                dst->dex = src->equip.dex;
                dst->int_ = src->equip.int_;
                dst->luk = src->equip.luk;
                dst->hp = src->equip.hp;
                dst->mp = src->equip.mp;
                dst->atk = src->equip.atk;
                dst->matk = src->equip.matk;
                dst->def = src->equip.def;
                dst->mdef = src->equip.mdef;
                dst->acc = src->equip.acc;
                dst->avoid = src->equip.avoid;
                //dst->hands = src->equip.hands;
                dst->hands = 0;
                dst->speed = src->equip.speed;
                dst->jump = src->equip.jump;
                dst->cash = wz_get_equip_info(dst->item.itemId)->cash;
            }

            for (uint8_t i = 0; i < 4; i++) {
                for (uint8_t j = 0; j < chr->inventory[i].slotCount; j++)
                    chr->inventory[i].items[j].isEmpty = true;
            }

            for (uint16_t i = 0; i < res->getCharacter.itemCount; i++) {
                uint8_t inv = res->getCharacter.inventoryItems[i].item.itemId / 1000000 - 2;
                chr->inventory[inv].items[res->getCharacter.inventoryItems[i].slot].isEmpty = false;
                struct InventoryItem *item = &chr->inventory[inv].items[res->getCharacter.inventoryItems[i].slot].item;
                item->item.id = res->getCharacter.inventoryItems[i].item.id;
                item->item.itemId = res->getCharacter.inventoryItems[i].item.itemId;
                item->item.flags = res->getCharacter.inventoryItems[i].item.flags;
                item->item.ownerLength = 0;
                item->item.giftFromLength = 0;
                item->quantity = res->getCharacter.inventoryItems[i].count;
            }

            chr->storage.id = res->getCharacter.storage.id;
            chr->storage.slots = res->getCharacter.storage.slots;
            chr->storage.count = res->getCharacter.storageItemCount + res->getCharacter.storageEquipCount;
            chr->storage.mesos = res->getCharacter.storage.mesos;

            for (size_t i = 0; i < res->getCharacter.storageEquipCount; i++) {
                chr->storage.storage[res->getCharacter.storageEquipment[i].slot].isEquip = true;
                const struct DatabaseEquipment *src = &res->getCharacter.storageEquipment[i].equip;
                struct Equipment *dst = &chr->storage.storage[res->getCharacter.storageEquipment[i].slot].equip;
                dst->id = src->id;
                dst->equipId = 0;
                dst->item.id = src->item.id;
                dst->item.itemId = src->item.itemId;
                //equip->item.cashId;
                //equip->item.sn; // What is this?

                dst->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
                                             //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
                dst->item.flags = src->item.flags;
                dst->item.expiration = -1; //equip->item.expiration = res->getCharacter.equippedEquipment[i].expiration;
                dst->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
                                                //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
                dst->level = src->level;
                dst->slots = src->slots;
                dst->str = src->str;
                dst->dex = src->dex;
                dst->int_ = src->int_;
                dst->luk = src->luk;
                dst->hp = src->hp;
                dst->mp = src->mp;
                dst->atk = src->atk;
                dst->matk = src->matk;
                dst->def = src->def;
                dst->mdef = src->mdef;
                dst->acc = src->acc;
                dst->avoid = src->avoid;
                //dst->hands = src->equip.hands;
                dst->hands = 0;
                dst->speed = src->speed;
                dst->jump = src->jump;
                dst->cash = wz_get_equip_info(dst->item.itemId)->cash;
            }

            for (size_t i = 0; i < res->getCharacter.storageItemCount; i++) {
                chr->storage.storage[res->getCharacter.storageItems[i].slot].isEquip = false;
                const struct DatabaseItem *src = &res->getCharacter.storageItems[i].item;
                struct InventoryItem *dst = &chr->storage.storage[res->getCharacter.storageItems[i].slot].item;
                dst->quantity = res->getCharacter.storageItems[i].count;
                dst->item.id = src->id;
                dst->item.itemId = src->itemId;
                dst->item.ownerLength = src->ownerLength;
                memcpy(dst->item.owner, src->owner, src->ownerLength);
                dst->item.flags = src->flags;
                dst->item.expiration = -1;
                dst->item.giftFromLength = 0;
            }

            chr->activeProjectile = -1;

            if (!chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].isEmpty) {
                // Bow
                if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 145) {
                    for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
                        if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2060) {
                            chr->activeProjectile = i;
                            break;
                        }
                    }
                    // Crossbow
                } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 146) {
                    for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
                        if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2061) {
                            chr->activeProjectile = i;
                            break;
                        }
                    }
                    // Claw
                } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 147) {
                    for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
                        if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_THROWING_STAR(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                            chr->activeProjectile = i;
                            break;
                        }
                    }
                    // Gun
                } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 149) {
                    for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
                        if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_BULLET(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                            chr->activeProjectile = i;
                            break;
                        }
                    }
                }
                // TODO: Capsules
            }

            chr->quests = hash_set_u16_create(sizeof(struct Quest), offsetof(struct Quest, id));
            if (chr->quests == NULL) {
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { -1 };
            }

            chr->monsterQuests = hash_set_u32_create(sizeof(struct MonsterRefCount), offsetof(struct MonsterRefCount, id));
            if (chr->monsterQuests == NULL) {
                hash_set_u16_destroy(chr->quests);
                chr->quests = NULL;
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { -1 };
            }

            chr->itemQuests = hash_set_u32_create(sizeof(uint32_t), 0);
            if (chr->itemQuests == NULL) {
                hash_set_u32_destroy(chr->monsterQuests);
                chr->monsterQuests = NULL;
                hash_set_u16_destroy(chr->quests);
                chr->quests = NULL;
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { -1 };
            }

            for (size_t i = 0; i < res->getCharacter.questCount; i++) {
                struct Quest quest = {
                    .id = res->getCharacter.quests[i],
                    .progressCount = 0,
                };
                hash_set_u16_insert(chr->quests, &quest); // TODO: Check

                const struct QuestInfo *quest_info = wz_get_quest_info(res->getCharacter.quests[i]);
                for (size_t i = 0; i < quest_info->endRequirementCount; i++) {
                    if (quest_info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_ITEM) {
                        const struct ItemInfo *item_info = wz_get_item_info(quest_info->endRequirements[i].item.id);
                        if (item_info->quest) {
                            int32_t total = 0;
                            uint8_t inv = quest_info->endRequirements[i].item.id / 1000000;
                            if (inv == 1) {
                                for (uint8_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
                                    if (!chr->equipmentInventory.items[j].isEmpty &&
                                            chr->equipmentInventory.items[j].equip.item.itemId == quest_info->endRequirements[i].item.id) {
                                        total++;
                                    }
                                }
                            } else {
                                inv -= 2;
                                for (uint8_t j = 0; j < chr->inventory[inv].slotCount; j++) {
                                    if (!chr->inventory[inv].items[j].isEmpty &&
                                            chr->inventory[inv].items[j].item.item.itemId == quest_info->endRequirements[i].item.id) {
                                        total += chr->inventory[inv].items[j].item.quantity;
                                    }
                                }
                            }

                            if (total < quest_info->endRequirements[i].item.count)
                                hash_set_u32_insert(chr->itemQuests, &quest_info->endRequirements[i].item.id);
                        }
                    }
                }
            }


            for (size_t i = 0; i < res->getCharacter.progressCount; i++) {
                struct Quest *quest = hash_set_u16_get(chr->quests, res->getCharacter.progresses[i].questId); // TODO: Check
                const struct QuestInfo *info = wz_get_quest_info(quest->id);
                uint8_t j;
                int16_t amount;
                for (size_t i_ = 0; true; i_++) {
                    struct QuestRequirement *req = &info->endRequirements[i_];
                    if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
                        for (size_t i_ = 0; true; i_++) {
                            if (req->mob.mobs[i_].id == res->getCharacter.progresses[i].progressId) {
                                j = i_;
                                amount = req->mob.mobs[i_].count;
                                break;
                            }
                        }
                        break;
                    }
                }

                if (res->getCharacter.progresses[i].progress < amount) {
                    struct MonsterRefCount *m = hash_set_u32_get(chr->monsterQuests, res->getCharacter.progresses[i].progressId);
                    if (m != NULL) {
                        m->refCount++;
                    } else {
                        struct MonsterRefCount new = {
                            .id = res->getCharacter.progresses[i].progressId,
                            .refCount = 1
                        };
                        hash_set_u32_insert(chr->monsterQuests, &new);
                    }
                }

                quest->progress[j] = res->getCharacter.progresses[i].progress;
                quest->progressCount++;
            }

            chr->questInfos = hash_set_u16_create(sizeof(struct QuestInfoProgress), offsetof(struct QuestInfoProgress, id));
            if (chr->questInfos == NULL) {
                hash_set_u32_destroy(chr->itemQuests);
                chr->itemQuests = NULL;
                hash_set_u32_destroy(chr->monsterQuests);
                chr->monsterQuests = NULL;
                hash_set_u16_destroy(chr->quests);
                chr->quests = NULL;
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { -1 };
            }

            for (size_t i = 0; i < res->getCharacter.questInfoCount; i++) {
                struct QuestInfoProgress new = {
                    .id = res->getCharacter.questInfos[i].infoId,
                    .length = res->getCharacter.questInfos[i].progressLength,
                };

                memcpy(new.value, res->getCharacter.questInfos[i].progress, res->getCharacter.questInfos[i].progressLength);

                hash_set_u16_insert(chr->questInfos, &new);
            }

            chr->completedQuests = hash_set_u16_create(sizeof(struct CompletedQuest), offsetof(struct CompletedQuest, id));
            if (chr->completedQuests == NULL) {
                hash_set_u16_destroy(chr->questInfos);
                chr->questInfos = NULL;
                hash_set_u32_destroy(chr->itemQuests);
                chr->itemQuests = NULL;
                hash_set_u32_destroy(chr->monsterQuests);
                chr->monsterQuests = NULL;
                hash_set_u16_destroy(chr->quests);
                chr->quests = NULL;
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { -1 };
            }

            for (size_t i = 0; i < res->getCharacter.completedQuestCount; i++) {
                struct tm tm = {
                    .tm_sec = res->getCharacter.completedQuests[i].time.second,
                    .tm_min = res->getCharacter.completedQuests[i].time.minute,
                    .tm_hour = res->getCharacter.completedQuests[i].time.hour,
                    .tm_mday = res->getCharacter.completedQuests[i].time.day,
                    .tm_mon = res->getCharacter.completedQuests[i].time.month - 1,
                    .tm_year = res->getCharacter.completedQuests[i].time.year - 1900,
                    .tm_gmtoff = 0,
                    .tm_isdst = 0
                };
                struct CompletedQuest quest = {
                    .id = res->getCharacter.completedQuests[i].id,
                    .time = timegm(&tm)
                };
                hash_set_u16_insert(chr->completedQuests, &quest); // TODO: Check
            }

            chr->skills = hash_set_u32_create(sizeof(struct Skill), offsetof(struct Skill, id));
            if (chr->skills == NULL) {
                hash_set_u16_destroy(chr->completedQuests);
                chr->completedQuests = NULL;
                hash_set_u16_destroy(chr->questInfos);
                chr->questInfos = NULL;
                hash_set_u32_destroy(chr->itemQuests);
                chr->itemQuests = NULL;
                hash_set_u32_destroy(chr->monsterQuests);
                chr->monsterQuests = NULL;
                hash_set_u16_destroy(chr->quests);
                chr->quests = NULL;
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { -1 };
            }

            for (size_t i = 0; i < res->getCharacter.skillCount; i++) {
                struct Skill skill = {
                    .id = res->getCharacter.skills[i].id,
                    .level = res->getCharacter.skills[i].level,
                    .masterLevel = res->getCharacter.skills[i].masterLevel,
                };
                hash_set_u32_insert(chr->skills, &skill); // TODO: Check
            }

            chr->monsterBook = hash_set_u32_create(sizeof(struct MonsterBookEntry), offsetof(struct MonsterBookEntry, id));
            if (chr->monsterBook == NULL) {
                hash_set_u32_destroy(chr->skills);
                chr->skills = NULL;
                hash_set_u16_destroy(chr->completedQuests);
                chr->completedQuests = NULL;
                hash_set_u16_destroy(chr->questInfos);
                chr->questInfos = NULL;
                hash_set_u32_destroy(chr->itemQuests);
                chr->itemQuests = NULL;
                hash_set_u32_destroy(chr->monsterQuests);
                chr->monsterQuests = NULL;
                hash_set_u16_destroy(chr->quests);
                chr->quests = NULL;
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { -1 };
            }

            for (size_t i = 0; i < res->getCharacter.monsterBookEntryCount; i++) {
                struct MonsterBookEntry entry = {
                    .id = res->getCharacter.monsterBook[i].id,
                    .count = res->getCharacter.monsterBook[i].quantity,
                };
                hash_set_u32_insert(chr->monsterBook, &entry);
            }

            memset(chr->keyMap, 0, KEYMAP_MAX_KEYS * sizeof(struct KeyMapEntry));
            for (size_t i = 0; i < res->getCharacter.keyMapEntryCount; i++) {
                chr->keyMap[res->getCharacter.keyMap[i].key].type = res->getCharacter.keyMap[i].type;
                chr->keyMap[res->getCharacter.keyMap[i].key].action = res->getCharacter.keyMap[i].action;
            }

            database_request_destroy(client->request);
            database_connection_unlock(client->conn);

            {
                uint8_t packet[SET_FIELD_PACKET_MAX_LENGTH];
                size_t len = set_field_packet(chr, packet);
                session_write(client->session, len, packet);
            }

            {
                uint8_t packet[KEYMAP_PACKET_LENGTH];
                keymap_packet(chr->keyMap, packet);
                session_write(client->session, KEYMAP_PACKET_LENGTH, packet);
            }

            session_write(client->session, 3, (uint8_t[]) { 0x9F, 0x00, 0x00 }); // Quickslot init
            session_write(client->session, 3, (uint8_t[]) { 0x7C, 0x00, 0x00 }); // Macro init
            session_write(client->session, 6, (uint8_t[]) { 0x50, 0x01, 0x00, 0x00, 0x00, 0x00 }); // Auto HP
            session_write(client->session, 6, (uint8_t[]) { 0x51, 0x01, 0x00, 0x00, 0x00, 0x00 }); // Auto MP
            session_write(client->session, 4, (uint8_t[]) { 0x3F, 0x00, 0x07, 0x00 }); // Buddylist

            {
                uint8_t packet[SET_GENDER_PACKET_LENGTH];
                set_gender_packet(chr->gender, packet);
                session_write(client->session, SET_GENDER_PACKET_LENGTH, packet);
            }

            session_write(client->session, 3, (uint8_t[]) { 0x2F, 0x00, 0x01 }); // Claim status changed?

            insert_client(chr->nameLength, chr->name, chr->id);

            client->handlerType = PACKET_TYPE_NONE;

            return (struct ClientContResult) { 0 };
        }
    break;

    case PACKET_TYPE_LOGOUT:
        if (client->databaseState == 0) {
            // If the client doesn't have a room - meaning it didn't load a character,
            // there is no need to flush the character.
            if (session_get_room(client->session) == NULL) {
                client_destroy(client);
                return (struct ClientContResult) { .status = 0 };
            }

            int fd = database_connection_lock(client->conn);
            if (fd == -2) {
                client->databaseState++;
            } else if (fd == -1) {
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            } else {
                client->databaseState++;
                return (struct ClientContResult) { .status = POLLIN, .fd = fd };
            }
        }

        if (client->databaseState == 1) {
            struct RequestParams params = {
                .type = DATABASE_REQUEST_TYPE_ALLOCATE_IDS,
                .allocateIds = {
                    .id = chr->id,
                    .accountId = chr->accountId,
                    .itemCount = 0,
                    .equippedCount = 0,
                    .equipCount = 0,
                    .storageItemCount = 0,
                    .storageEquipCount = 0
                },
            };

            for (size_t i = 0; i < 4; i++) {
                for (size_t j = 0; j < chr->inventory[i].slotCount; j++) {
                    if (!chr->inventory[i].items[j].isEmpty && chr->inventory[i].items[j].item.item.id == 0) {
                        params.allocateIds.items[params.allocateIds.itemCount] = chr->inventory[i].items[j].item.item.itemId;
                        params.allocateIds.itemCount++;
                    }
                }
            }

            for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
                if (!chr->equippedEquipment[i].isEmpty && chr->equippedEquipment[i].equip.id == 0) {
                    params.allocateIds.equippedEquipment[params.allocateIds.equippedCount].id =
                        chr->equippedEquipment[i].equip.item.id;
                    params.allocateIds.equippedEquipment[params.allocateIds.equippedCount].equipId =
                        chr->equippedEquipment[i].equip.equipId;
                    params.allocateIds.equippedEquipment[params.allocateIds.equippedCount].itemId =
                        chr->equippedEquipment[i].equip.item.itemId;
                    params.allocateIds.equippedCount++;
                }
            }

            for (size_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
                if (!chr->equipmentInventory.items[i].isEmpty && chr->equipmentInventory.items[i].equip.id == 0) {
                    params.allocateIds.equippedEquipment[params.allocateIds.equipCount].id =
                        chr->equipmentInventory.items[i].equip.item.id;
                    params.allocateIds.equippedEquipment[params.allocateIds.equipCount].equipId =
                        chr->equipmentInventory.items[i].equip.equipId;
                    params.allocateIds.equippedEquipment[params.allocateIds.equipCount].itemId =
                        chr->equipmentInventory.items[i].equip.item.itemId;
                    params.allocateIds.equipCount++;
                }
            }

            client->request = database_request_create(client->conn, &params);
            if (client->request == NULL) {
                database_connection_unlock(client->conn);
                return (struct ClientContResult) { .status = -1 };
            }

            status = database_request_execute(client->request, 0);
            client->databaseState++;
            if (status > 0) {
                return (struct ClientContResult) { .status = status, .fd = database_connection_get_fd(client->conn) };
            } else if (status < 0) {
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                session_close_event(client->session);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            } else {
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                client->databaseState++;
            }
        }

        if (client->databaseState == 2) {
            status = database_request_execute(client->request, status);
            if (status > 0) {
                return (struct ClientContResult) { .status = status, .fd = database_connection_get_fd(client->conn) };
            } else if (status < 0) {
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                session_close_event(client->session);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            } else {
                client->databaseState++;
            }
        }

        if (client->databaseState == 3) {
            const struct RequestParams *out_params = database_request_get_params(client->request);
            const union DatabaseResult *res = database_request_result(client->request);
            size_t count = 0;
            for (size_t i = 0; i < 4; i++) {
                for (size_t j = 0; j < chr->inventory[i].slotCount; j++) {
                    if (!chr->inventory[i].items[j].isEmpty && chr->inventory[i].items[j].item.item.id == 0) {
                        chr->inventory[i].items[j].item.item.id = res->allocateIds.items[count];
                        count++;
                    }
                }
            }

            count = 0;
            for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
                if (!chr->equippedEquipment[i].isEmpty && chr->equippedEquipment[i].equip.id == 0) {
                    chr->equippedEquipment[i].equip.item.id = out_params->allocateIds.equippedEquipment[count].id;
                    chr->equippedEquipment[i].equip.equipId = out_params->allocateIds.equippedEquipment[count].equipId;
                    chr->equippedEquipment[i].equip.id = res->allocateIds.equippedEquipment[count];
                    count++;
                }
            }

            count = 0;
            for (size_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
                if (!chr->equipmentInventory.items[i].isEmpty && chr->equipmentInventory.items[i].equip.id == 0) {
                    chr->equipmentInventory.items[i].equip.item.id = out_params->allocateIds.equipmentInventory[count].id;
                    chr->equipmentInventory.items[i].equip.equipId = out_params->allocateIds.equipmentInventory[count].equipId;
                    chr->equipmentInventory.items[i].equip.id = res->allocateIds.equipmentInventory[count];
                    count++;
                }
            }

            database_request_destroy(client->request);

            struct RequestParams params = {
                .type = DATABASE_REQUEST_TYPE_UPDATE_CHARACTER,
                .updateCharacter = {
                    .id = chr->id,
                    .map = wz_get_map_forced_return(chr->map),
                    .spawnPoint = chr->spawnPoint,
                    .job = chr->job,
                    .level = chr->level,
                    .exp = chr->exp,
                    .maxHp = chr->maxHp,
                    .hp = chr->hp,
                    .maxMp = chr->maxMp,
                    .mp = chr->mp,
                    .str = chr->str,
                    .dex = chr->dex,
                    .int_ = chr->int_,
                    .luk = chr->luk,
                    .ap = chr->ap,
                    .sp = chr->sp,
                    .fame = chr->fame,
                    .skin = chr->skin,
                    .face = chr->face,
                    .hair = chr->hair,
                    .mesos = chr->mesos,
                    .equipSlots = chr->equipmentInventory.slotCount,
                    .useSlots = chr->inventory[0].slotCount,
                    .setupSlots = chr->inventory[1].slotCount,
                    .etcSlots = chr->inventory[2].slotCount,
                    .equippedCount = 0,
                    .equipCount = 0,
                    .itemCount = 0,
                }
            };

            for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
                if (!chr->equippedEquipment[i].isEmpty) {
                    struct Equipment *src = &chr->equippedEquipment[i].equip;
                    struct DatabaseCharacterEquipment *dst = &params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount];
                    dst->id = src->id;
                    dst->equip.id = src->equipId;
                    dst->equip.item.id = src->item.id;
                    dst->equip.item.itemId = src->item.itemId;
                    dst->equip.item.flags = src->item.flags;
                    dst->equip.item.ownerLength = src->item.ownerLength;
                    memcpy(dst->equip.item.owner, src->item.owner, src->item.ownerLength);
                    dst->equip.item.giverLength = src->item.giftFromLength;
                    memcpy(dst->equip.item.giver, src->item.giftFrom, src->item.giftFromLength);


                    dst->equip.level = src->level;
                    dst->equip.slots = src->slots;
                    dst->equip.str = src->str;
                    dst->equip.dex = src->dex;
                    dst->equip.int_ = src->int_;
                    dst->equip.luk = src->luk;
                    dst->equip.hp = src->hp;
                    dst->equip.mp = src->mp;
                    dst->equip.atk = src->atk;
                    dst->equip.matk = src->matk;
                    dst->equip.def = src->def;
                    dst->equip.mdef = src->mdef;
                    dst->equip.acc = src->acc;
                    dst->equip.avoid = src->avoid;
                    dst->equip.speed = src->speed;
                    dst->equip.jump = src->jump;

                    params.updateCharacter.equippedCount++;
                }
            }

            for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
                if (!chr->equipmentInventory.items[i].isEmpty) {
                    struct Equipment *src = &chr->equipmentInventory.items[i].equip;
                    struct DatabaseCharacterEquipment *dst = &params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip;
                    dst->id = src->id;
                    dst->equip.id = src->equipId;
                    params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].slot = i;
                    dst->equip.item.id = src->item.id;
                    dst->equip.item.itemId = src->item.itemId;
                    dst->equip.item.flags = src->item.flags;
                    dst->equip.item.ownerLength = src->item.ownerLength;
                    memcpy(dst->equip.item.owner, src->item.owner, src->item.ownerLength);
                    dst->equip.item.giverLength = src->item.giftFromLength;
                    memcpy(dst->equip.item.giver, src->item.giftFrom, src->item.giftFromLength);


                    dst->equip.level = src->level;
                    dst->equip.slots = src->slots;
                    dst->equip.str = src->str;
                    dst->equip.dex = src->dex;
                    dst->equip.int_ = src->int_;
                    dst->equip.luk = src->luk;
                    dst->equip.hp = src->hp;
                    dst->equip.mp = src->mp;
                    dst->equip.atk = src->atk;
                    dst->equip.matk = src->matk;
                    dst->equip.def = src->def;
                    dst->equip.mdef = src->mdef;
                    dst->equip.acc = src->acc;
                    dst->equip.avoid = src->avoid;
                    dst->equip.speed = src->speed;
                    dst->equip.jump = src->jump;

                    params.updateCharacter.equipCount++;
                }
            }

            for (uint8_t inv = 0; inv < 4; inv++) {
                for (uint8_t i = 0; i < chr->inventory[inv].slotCount; i++) {
                    if (!chr->inventory[inv].items[i].isEmpty) {
                        struct Item *src = &chr->inventory[inv].items[i].item.item;
                        struct DatabaseItem *dst = &params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item;
                        params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].slot = i;
                        params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].count = chr->inventory[inv].items[i].item.quantity;

                        dst->id = src->id;
                        dst->itemId = src->itemId;
                        dst->flags = src->flags;
                        dst->ownerLength = src->ownerLength;
                        memcpy(dst->owner, src->owner, src->ownerLength);
                        dst->giverLength = src->giftFromLength;
                        memcpy(dst->giver, src->giftFrom, src->giftFromLength);

                        params.updateCharacter.itemCount++;
                    }
                }
            }

            params.updateCharacter.quests = malloc(hash_set_u16_size(chr->quests) * sizeof(uint16_t));
            if (params.updateCharacter.quests == NULL) {
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            }

            struct AddQuestContext ctx = {
                .quests = params.updateCharacter.quests,
                .currentQuest = 0,
                .progressCount = 0
            };
            hash_set_u16_foreach(chr->quests, add_quest, &ctx);

            params.updateCharacter.questCount = ctx.currentQuest;

            params.updateCharacter.progresses = malloc(ctx.progressCount * sizeof(struct DatabaseProgress));
            if (params.updateCharacter.progresses == NULL) {
                free(params.updateCharacter.quests);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            }

            struct AddProgressContext ctx2 = {
                .progresses = params.updateCharacter.progresses,
                .currentProgress = 0
            };
            hash_set_u16_foreach(chr->quests, add_progress, &ctx2);

            params.updateCharacter.progressCount = ctx2.currentProgress;

            params.updateCharacter.questInfos = malloc(hash_set_u16_size(chr->questInfos) * sizeof(struct DatabaseInfoProgress));
            if (params.updateCharacter.questInfos == NULL) {
                free(params.updateCharacter.progresses);
                free(params.updateCharacter.quests);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            }

            struct AddQuestInfoContext ctx3 = {
                .infos = params.updateCharacter.questInfos,
                .currentInfo = 0,
            };
            hash_set_u16_foreach(chr->questInfos, add_quest_info, &ctx3);

            params.updateCharacter.questInfoCount = ctx3.currentInfo;

            params.updateCharacter.completedQuests = malloc(hash_set_u16_size(chr->completedQuests) * sizeof(struct DatabaseCompletedQuest));
            if (params.updateCharacter.completedQuests == NULL) {
                free(params.updateCharacter.questInfos);
                free(params.updateCharacter.progresses);
                free(params.updateCharacter.quests);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            }

            struct AddCompletedQuestContext ctx4 = {
                .quests = params.updateCharacter.completedQuests,
                .currentQuest = 0,
            };
            hash_set_u16_foreach(chr->completedQuests, add_completed_quest, &ctx4);

            params.updateCharacter.completedQuestCount = ctx4.currentQuest;

            params.updateCharacter.skills = malloc(hash_set_u32_size(chr->skills) * sizeof(struct DatabaseSkill));
            if (params.updateCharacter.completedQuests == NULL) {
                free(params.updateCharacter.completedQuests);
                free(params.updateCharacter.questInfos);
                free(params.updateCharacter.progresses);
                free(params.updateCharacter.quests);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            }

            struct AddSkillContext ctx5 = {
                .skills = params.updateCharacter.skills,
                .currentSkill = 0,
            };
            hash_set_u32_foreach(chr->skills, add_skill, &ctx5);

            params.updateCharacter.skillCount = ctx5.currentSkill;

            params.updateCharacter.monsterBook = malloc(hash_set_u32_size(chr->monsterBook) * sizeof(struct DatabaseMonsterBookEntry));
            if (params.updateCharacter.monsterBook == NULL) {
                free(params.updateCharacter.skills);
                free(params.updateCharacter.completedQuests);
                free(params.updateCharacter.questInfos);
                free(params.updateCharacter.progresses);
                free(params.updateCharacter.quests);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            }

            struct AddMonsterBookContext ctx6 = {
                .monsterBook = params.updateCharacter.monsterBook,
                .currentEntry = 0,
            };
            hash_set_u32_foreach(chr->monsterBook, add_monster_book_entry, &ctx6);

            params.updateCharacter.monsterBookEntryCount = ctx6.currentEntry;

            uint8_t key_count = 0;
            for (uint8_t i = 0; i < KEYMAP_MAX_KEYS; i++) {
                if (chr->keyMap[i].type != 0)
                    key_count++;
            }

            params.updateCharacter.keyMap = malloc(key_count * sizeof(struct DatabaseKeyMapEntry));
            if (params.updateCharacter.keyMap == NULL) {
                free(params.updateCharacter.monsterBook);
                free(params.updateCharacter.skills);
                free(params.updateCharacter.completedQuests);
                free(params.updateCharacter.questInfos);
                free(params.updateCharacter.progresses);
                free(params.updateCharacter.quests);
                client_destroy(client);
                return (struct ClientContResult) { .status = -1 };
            }

            params.updateCharacter.keyMapEntryCount = 0;
            for (uint8_t i = 0; i < KEYMAP_MAX_KEYS; i++) {
                if (chr->keyMap[i].type != 0) {
                    params.updateCharacter.keyMap[params.updateCharacter.keyMapEntryCount].key = i;
                    params.updateCharacter.keyMap[params.updateCharacter.keyMapEntryCount].type = chr->keyMap[i].type;
                    params.updateCharacter.keyMap[params.updateCharacter.keyMapEntryCount].action = chr->keyMap[i].action;
                    params.updateCharacter.keyMapEntryCount++;
                }
            }

            client->request = database_request_create(client->conn, &params);
            if (client->request == NULL) {
                free(params.updateCharacter.keyMap);
                free(params.updateCharacter.monsterBook);
                free(params.updateCharacter.skills);
                free(params.updateCharacter.completedQuests);
                free(params.updateCharacter.questInfos);
                free(params.updateCharacter.progresses);
                free(params.updateCharacter.quests);
                client_destroy(client);
                return (struct ClientContResult) { -1 };
            }

            status = database_request_execute(client->request, 0);
            client->databaseState++;
            if (status <= 0) {
                free(params.updateCharacter.keyMap);
                free(params.updateCharacter.monsterBook);
                free(params.updateCharacter.skills);
                free(params.updateCharacter.completedQuests);
                free(params.updateCharacter.questInfos);
                free(params.updateCharacter.progresses);
                free(params.updateCharacter.quests);
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                client_destroy(client);
                return (struct ClientContResult) { status };
            }

            return (struct ClientContResult) { status, database_connection_get_fd(client->conn) };
        }

        if (client->databaseState == 4) {
            status = database_request_execute(client->request, status);
            if (status <= 0) {
                const struct RequestParams *params = database_request_get_params(client->request);
                free(params->updateCharacter.keyMap);
                free(params->updateCharacter.monsterBook);
                free(params->updateCharacter.skills);
                free(params->updateCharacter.completedQuests);
                free(params->updateCharacter.questInfos);
                free(params->updateCharacter.progresses);
                free(params->updateCharacter.quests);
                database_request_destroy(client->request);
                database_connection_unlock(client->conn);
                client->handlerType = PACKET_TYPE_NONE;
                client_destroy(client);

                return (struct ClientContResult) { status };
            }

            return (struct ClientContResult) { status, database_connection_get_fd(client->conn) };
        }
    break;
    default:
        assert(0);
    }

    return (struct ClientContResult) { 0 };
}

void client_update_conn(struct Client *client, struct DatabaseConnection *conn)
{
    client->conn = conn;
}

void client_handle_command(struct Client *client, struct ClientCommand *cmd)
{
    switch (cmd->type) {
    case CLIENT_COMMAND_PARTY_INVITE: {
        if (client->party != NULL) {
            // We can reuse `cmd`
            cmd->type = CLIENT_COMMAND_PARTY_REJECT;
            cmd->reason = 1;
            cmd->id = find_id_by_name(cmd->nameLen, cmd->name);
            session_send_command(client->session, cmd->id, cmd);
            return;
        }

        uint8_t packet[PARTY_INVITE_PACKET_MAX_LENGTH];
        size_t len = party_invite_packet(cmd->id, cmd->nameLen, cmd->name, packet);
        session_write(client->session, len, packet);
        free(cmd);
    }
    break;

    case CLIENT_COMMAND_PARTY_REJECT: {
        if (cmd->reason == 0) {
            uint8_t packet[PARTY_STATUS_MESSAGE_PACKET_LENGTH];
            party_status_message_packet(23, packet);
            session_write(client->session, PARTY_STATUS_MESSAGE_PACKET_LENGTH, packet);
            free(cmd);
        } else if (cmd->reason == 1) {
            uint8_t packet[PARTY_STATUS_MESSAGE_PACKET_LENGTH];
            party_status_message_packet(16, packet);
            session_write(client->session, PARTY_STATUS_MESSAGE_PACKET_LENGTH, packet);
            free(cmd);
        }
    }
    break;
    }

}

void client_notify_command_received(struct Client *client, struct ClientCommand *cmd, bool sent)
{
    if (!sent) {
        if (cmd->type == CLIENT_COMMAND_PARTY_INVITE) {
            uint8_t packet[PARTY_STATUS_MESSAGE_PACKET_LENGTH];
            party_status_message_packet(19, packet);
            session_write(client->session, PARTY_STATUS_MESSAGE_PACKET_LENGTH, packet);
        }
        free(cmd);
    }

    // If the map doesn't match then it means that client_warp() was called
    if (client->character.map != room_get_id(session_get_room(client->session)))
        client_warp(client, client->character.map, client->character.spawnPoint);
}

const struct Character *client_get_character(struct Client *client)
{
    return &client->character;
}

uint32_t client_get_active_npc(struct Client *client)
{
    return client->npc;
}

uint16_t client_get_active_quest(struct Client *client)
{
    return client->qid;
}

struct MapHandleContainer *client_get_map(struct Client *client)
{
    return &client->map;
}

void client_announce_self_to_map(struct Client *client)
{
    uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
    size_t len = add_player_to_map_packet(client_get_character(client), packet);
    session_broadcast_to_room(client->session, len, packet);
}

void client_announce_add_player(struct Client *client, const struct Character *chr)
{
    uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
    size_t len = add_player_to_map_packet(chr, packet);
    session_write(client->session, len, packet);
}

void client_announce_add_npc(struct Client *client, const struct Npc *npc)
{
    {
        uint8_t packet[SPAWN_NPC_PACKET_LENGTH];
        spawn_npc_packet(npc->oid, npc->id, npc->x, npc->cy, npc->f == 1, npc->fh, npc->rx0, npc->rx1, packet);
        session_write(client->session, SPAWN_NPC_PACKET_LENGTH, packet);
    }
    {
        uint8_t packet[SPAWN_NPC_CONTROLLER_PACKET_LENGTH];
        spawn_npc_controller_packet(npc->oid, npc->id, npc->x, npc->cy, npc->f == 1, npc->fh, npc->rx0, npc->rx1, packet);
        session_write(client->session, SPAWN_NPC_CONTROLLER_PACKET_LENGTH, packet);
    }
}

void client_announce_monster(struct Client *client, const struct Monster *monster)
{
    uint8_t packet[SPAWN_MONSTER_PACKET_LENGTH];
    spawn_monster_packet(monster->oid, monster->id, monster->x, monster->y, monster->fh, false, packet);
    session_write(client->session, SPAWN_MONSTER_PACKET_LENGTH, packet);
}

bool client_announce_drop(struct Client *client, uint32_t owner_id, uint32_t dropper_oid, uint8_t type, bool player_drop, const struct Drop *drop)
{
    struct Character *chr = &client->character;
    if (hash_set_u32_insert(client->visibleMapObjects, &drop->oid) == -1)
        return false;

    if (type < 3 && owner_id == chr->id)
        type = 2;

    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[DROP_MESO_FROM_OBJECT_PACKET_LENGTH];
        drop_meso_from_object_packet(drop->oid, drop->meso, owner_id, type,
                drop->x, drop->y, drop->x, drop->y, dropper_oid, player_drop, packet);
        session_write(client->session, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_ITEM: {
        if (drop->qid != 0) {
            struct Quest *quest = hash_set_u16_get(client->character.quests, drop->qid);
            if (quest != NULL && hash_set_u32_get(chr->itemQuests, drop->item.item.itemId) != NULL) {
                uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
                drop_item_from_object_packet(drop->oid, drop->item.item.itemId, owner_id, type,
                    drop->x, drop->y, drop->x, drop->y, dropper_oid, player_drop, packet);
                session_write(client->session, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
            }
        } else {
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->item.item.itemId, owner_id, type,
                    drop->x, drop->y, drop->x, drop->y, dropper_oid, player_drop, packet);
            session_write(client->session, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
        }
    }
    break;

    case DROP_TYPE_EQUIP: {
            uint8_t packet[DROP_ITEM_FROM_OBJECT_PACKET_LENGTH];
            drop_item_from_object_packet(drop->oid, drop->equip.item.itemId, owner_id, type,
                    drop->x, drop->y, drop->x, drop->y, dropper_oid, player_drop, packet);
            session_write(client->session, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);
    }
    break;
    }

    return true;
}

bool client_announce_spawn_drop(struct Client *client, uint32_t owner_id, uint32_t dropper_oid, uint8_t type, bool player_drop, const struct Drop *drop)
{
    struct Character *chr = &client->character;
    if (hash_set_u32_insert(client->visibleMapObjects, &drop->oid) == -1)
        return false;

    if (type < 3 && owner_id == chr->id)
        type = 2;

    switch (drop->type) {
    case DROP_TYPE_MESO: {
        uint8_t packet[SPAWN_MESO_DROP_PACKET_LENGTH];
        spawn_meso_drop_packet(drop->oid, drop->meso, owner_id, type, drop->x, drop->y, player_drop, packet);
        session_write(client->session, SPAWN_MESO_DROP_PACKET_LENGTH, packet);
    }
    break;

    case DROP_TYPE_ITEM: {
        if (drop->qid != 0) {
            struct Quest *quest = hash_set_u16_get(client->character.quests, drop->qid);
            if (quest != NULL && hash_set_u32_get(chr->itemQuests, drop->item.item.itemId) != NULL) {
                uint8_t packet[SPAWN_ITEM_DROP_PACKET_LENGTH];
                spawn_item_drop_packet(drop->oid, drop->item.item.itemId, owner_id, type, drop->x, drop->y, player_drop, packet);
                session_write(client->session, SPAWN_ITEM_DROP_PACKET_LENGTH, packet);
            }
        } else {
            uint8_t packet[SPAWN_ITEM_DROP_PACKET_LENGTH];
            spawn_item_drop_packet(drop->oid, drop->item.item.itemId, owner_id, type, drop->x, drop->y, player_drop, packet);
            session_write(client->session, SPAWN_ITEM_DROP_PACKET_LENGTH, packet);
        }
    }
    break;

    case DROP_TYPE_EQUIP: {
            uint8_t packet[SPAWN_ITEM_DROP_PACKET_LENGTH];
            spawn_item_drop_packet(drop->oid, drop->equip.item.itemId, owner_id, type, drop->x, drop->y, player_drop, packet);
            session_write(client->session, SPAWN_ITEM_DROP_PACKET_LENGTH, packet);
    }
    break;
    }

    return true;

}

void client_update_player_pos(struct Client *client, int16_t x, int16_t y, uint16_t fh, uint8_t stance)
{
    client->character.x = x;
    client->character.y = y;
    client->character.fh = fh;
    client->character.stance = stance;
}

void client_set_hp(struct Client *client, int16_t hp)
{
    struct Character *chr = &client->character;
    if (hp == chr->hp)
        return;

    character_set_hp(chr, hp);
    client->stats |= STAT_HP;
}

void client_set_hp_now(struct Client *client, int16_t hp)
{
    struct Character *chr = &client->character;
    character_set_hp(chr, hp);
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value = { .i16 = chr->hp };
    size_t len = stat_change_packet(true, STAT_HP, &value, packet);
    session_write(client->session, len, packet);
}

void client_adjust_hp(struct Client *client, int32_t hp)
{
    // Overflow protection
    if (hp > 0 && client->character.hp > INT16_MAX - hp)
        hp = INT16_MAX - client->character.hp;
    else if (client->character.hp < INT16_MIN - hp)
        hp = INT16_MIN - client->character.hp;
    client_set_hp(client, client->character.hp + hp);
}

void client_adjust_hp_now(struct Client *client, int32_t hp)
{
    // Overflow protection
    if (hp > 0 && client->character.hp > INT16_MAX - hp)
        hp = INT16_MAX - client->character.hp;
    else if (client->character.hp < INT16_MIN - hp)
        hp = INT16_MIN - client->character.hp;
    client_set_hp_now(client, client->character.hp + hp);
}

void client_set_mp(struct Client *client, int16_t mp)
{
    struct Character *chr = &client->character;
    if (mp == chr->mp)
        return;

    character_set_mp(chr, mp);
    client->stats |= STAT_MP;
}

void client_set_mp_now(struct Client *client, int16_t mp)
{
    struct Character *chr = &client->character;
    character_set_mp(chr, mp);
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value = { .i16 = chr->mp };
    size_t len = stat_change_packet(true, STAT_MP, &value, packet);
    session_write(client->session, len, packet);
}

void client_adjust_mp(struct Client *client, int16_t mp)
{
    if (mp > 0 && client->character.mp > INT16_MAX - mp)
        mp = INT16_MAX - client->character.mp;
    client_set_mp(client, client->character.mp + mp);
}

void client_adjust_mp_now(struct Client *client, int32_t mp)
{
    // Overflow protection
    if (mp > 0 && client->character.mp > INT16_MAX - mp)
        mp = INT16_MAX - client->character.mp;
    else if (client->character.mp < INT16_MIN - mp)
        mp = INT16_MIN - client->character.mp;
    client_set_mp_now(client, client->character.mp + mp);
}

void client_adjust_sp(struct Client *client, int16_t sp)
{
    struct Character *chr = &client->character;
    if (sp > 0 && chr->sp > INT16_MAX - sp)
        sp = INT16_MAX - chr->sp;
    character_set_sp(chr, chr->sp + sp);
    client->stats |= STAT_SP;
}

#define DEFINE_STAT_ADJUST(name, stat_name, stat) \
    void client_adjust_##name(struct Client *client, int16_t amount) \
    { \
        if (amount == 0) \
            return; \
 \
        struct Character *chr = &client->character; \
        if (chr->ap >= amount) { \
            if (amount > INT16_MAX - chr->stat_name) \
                amount = INT16_MAX - chr->stat_name; \
            character_set_##name(chr, chr->stat_name + amount); \
            character_set_ap(chr, chr->ap - amount); \
            client->stats |= stat | STAT_AP; \
        } \
    }

DEFINE_STAT_ADJUST(str, str, STAT_STR)
DEFINE_STAT_ADJUST(dex, dex, STAT_DEX)
DEFINE_STAT_ADJUST(int, int_, STAT_INT)
DEFINE_STAT_ADJUST(luk, luk, STAT_LUK)

void client_commit_stats(struct Client *client)
{
    struct Character *chr = &client->character;
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    uint8_t value_count = 0;
    union StatValue value[20];
    if (client->stats & STAT_SKIN) {
        value[value_count].u8 = chr->skin;
        value_count++;
    }

    if (client->stats & STAT_FACE) {
        value[value_count].u32 = chr->face;
        value_count++;
    }

    if (client->stats & STAT_HAIR) {
        value[value_count].u32 = chr->hair;
        value_count++;
    }

    if (client->stats & STAT_LEVEL) {
        value[value_count].u8 = chr->level;
        value_count++;
    }

    if (client->stats & STAT_JOB) {
        value[value_count].u16 = chr->job;
        value_count++;
    }

    if (client->stats & STAT_STR) {
        value[value_count].i16 = chr->str;
        value_count++;
    }

    if (client->stats & STAT_DEX) {
        value[value_count].i16 = chr->dex;
        value_count++;
    }

    if (client->stats & STAT_INT) {
        value[value_count].i16 = chr->int_;
        value_count++;
    }

    if (client->stats & STAT_LUK) {
        value[value_count].i16 = chr->luk;
        value_count++;
    }

    if (client->stats & STAT_HP) {
        value[value_count].i16 = chr->hp;
        value_count++;
    }

    if (client->stats & STAT_MAX_HP) {
        value[value_count].i16 = chr->maxHp;
        value_count++;
    }

    if (client->stats & STAT_MP) {
        value[value_count].i16 = chr->mp;
        value_count++;
    }

    if (client->stats & STAT_MAX_MP) {
        value[value_count].i16 = chr->maxMp;
        value_count++;
    }

    if (client->stats & STAT_AP) {
        value[value_count].i16 = chr->ap;
        value_count++;
    }

    if (client->stats & STAT_SP) {
        value[value_count].i16 = chr->sp;
        value_count++;
    }

    if (client->stats & STAT_EXP) {
        value[value_count].i32 = chr->exp;
        value_count++;
    }

    if (client->stats & STAT_FAME) {
        value[value_count].i16 = chr->fame;
        value_count++;
    }

    if (client->stats & STAT_MESO) {
        value[value_count].i32 = chr->mesos;
        value_count++;
    }

    if (client->stats & STAT_PET) {
        //value[value_count].u8 = chr->pe;
        //value_count++;
    }

    if (client->stats & STAT_GACHA_EXP) {
        value[value_count].i32 = chr->gachaExp;
        value_count++;
    }

    size_t len = stat_change_packet(true, client->stats, value, packet);
    session_write(client->session, len, packet);
    client->stats = 0;
}

bool client_assign_sp(struct Client *client, uint32_t id)
{
    struct Character *chr = &client->character;

    const struct SkillInfo *info = wz_get_skill_info(id);
    if (info == NULL)
        return false;

    if (id >= 1000 && id <= 1002) {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, 0, NULL, packet);
        session_write(client->session, len, packet);
    } else if (chr->sp > 0) {
        chr->sp--;
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        union StatValue value = {
            .i16 = chr->sp,
        };
        size_t len = stat_change_packet(true, STAT_SP, &value, packet);
        session_write(client->session, len, packet);
    } else {
        return true;
    }

    struct Skill *skill = hash_set_u32_get(chr->skills, id);
    if (skill == NULL) {
        uint16_t job = id / 10000;
        if ((job % 1000 != 0 && chr->job / 100 != job / 100) || (job / 10 % 10 != 0 && chr->job % 10 < job % 10))
            return false;

        if (info->reqId != 0) {
            struct Skill *req = hash_set_u32_get(chr->skills, info->reqId);
            if (req == NULL || req->level < info->reqLevel)
                return false;
        }

        struct Skill new = {
            .id = id,
            .level = 1,
            .masterLevel = ((id / 10000) % 10 == 2) ? 0 : info->levelCount,
        };

        hash_set_u32_insert(chr->skills, &new);
        skill = hash_set_u32_get(chr->skills, id);
    } else if (skill->level < skill->masterLevel) {
        skill->level++;
    } else {
        return true;
    }

    {
        uint8_t packet[UPDATE_SKILL_PACKET_LENGTH];
        update_skill_packet(id, skill->level, skill->masterLevel, packet);
        session_write(client->session, UPDATE_SKILL_PACKET_LENGTH, packet);
    }

    return true;
}

void client_gain_exp(struct Client *client, int32_t exp, bool reward)
{
    struct Character *chr = &client->character;
    if (!reward) {
        uint8_t packet[EXP_GAIN_PACKET_LENGTH];
        exp_gain_packet(exp, 0, 0, true, packet);
        session_write(client->session, EXP_GAIN_PACKET_LENGTH, packet);
    } else {
        uint8_t packet[EXP_GAIN_IN_CHAT_PACKET_LENGTH];
        exp_gain_in_chat_packet(exp, 0, 0, true, packet);
        session_write(client->session, EXP_GAIN_IN_CHAT_PACKET_LENGTH, packet);
    }

    client->stats |= STAT_EXP;

    do {
        exp = character_gain_exp(chr, exp);
        if (exp < 0)
            break;

        if ((chr->job == JOB_BEGINNER || chr->job == JOB_NOBLESSE || chr->job == JOB_LEGEND) && chr->level <= 10) {
            if (chr->level <= 5) {
                chr->str += 5;
                client->stats |= STAT_STR;
            } else {
                chr->str += 4;
                chr->dex += 1;
                client->stats |= STAT_STR | STAT_DEX;
            }
        } else {
            if (chr->job != JOB_BEGINNER && chr->job != JOB_NOBLESSE && chr->job != JOB_LEGEND) {
                chr->sp += 3;
                client->stats |= STAT_SP;
            }

            int8_t ap = 5;
            if (job_type(chr->job) == JOB_TYPE_CYGNUS) {
                if (chr->level > 10) {
                    if (chr->level <= 17)
                        ap += 2;
                    else if (chr->level < 77)
                        ap++;
                }
            }
            character_set_ap(chr, chr->ap + ap); // TODO: Overflow protection
            client->stats |= STAT_AP;
        }

        if (chr->job == JOB_BEGINNER || chr->job == JOB_NOBLESSE || chr->job == JOB_LEGEND) {
            character_set_max_hp(chr, chr->maxHp + rand() % 5 + 12);
            character_set_max_mp(chr, chr->maxMp + rand() % 3 + 10);
        } else if (job_is_a(chr->job, JOB_FIGHTER) || job_is_a(chr->job, JOB_DAWN_WARRIOR)) {
            character_set_max_hp(chr, chr->maxHp + rand() % 5 + 24);
            character_set_max_mp(chr, chr->maxMp + rand() % 3 + 4);
        } else if (job_is_a(chr->job, JOB_MAGICIAN) || job_is_a(chr->job, JOB_BLAZE_WIZARD)) {
            character_set_max_hp(chr, chr->maxHp + rand() % 5 + 10);
            character_set_max_mp(chr, chr->maxMp + rand() % 3 + 22);

            if (job_is_a(chr->job, JOB_MAGICIAN)) {
                struct Skill *improved_max_mp_increase = hash_set_u32_get(chr->skills, 2000001);
                if (improved_max_mp_increase != NULL)
                    character_set_max_mp(chr, chr->maxMp + improved_max_mp_increase->level * 2);
            } else {
                struct Skill *increasing_max_mp = hash_set_u32_get(chr->skills, 12000000);
                if (increasing_max_mp != NULL)
                    character_set_max_mp(chr, chr->maxMp + increasing_max_mp->level * 2);
            }
        } else if (job_is_a(chr->job, JOB_ARCHER) || job_is_a(chr->job, JOB_ROGUE) ||
                job_is_a(chr->job, JOB_WIND_ARCHER) || job_is_a(chr->job, JOB_NIGHT_WALKER)) {
            character_set_max_hp(chr, chr->maxHp + rand() % 5 + 20);
            character_set_max_mp(chr, chr->maxMp + rand() % 3 + 14);
        } else if (job_is_a(chr->job, JOB_PIRATE) || job_is_a(chr->job, JOB_THUNDER_BREAKER)) {
            character_set_max_hp(chr, chr->maxHp + rand() % 7 + 22);
            character_set_max_mp(chr, chr->maxMp + rand() % 6 + 18);
        } else if (job_is_a(chr->job, JOB_ARAN)) {
            character_set_max_hp(chr, chr->maxHp + rand() % 5 + 44);
            character_set_max_mp(chr, chr->maxMp + rand() % 5 + 4);
        }
        character_set_max_mp(chr, chr->maxMp + character_get_effective_int(chr) /
                (job_is_a(chr->job, JOB_MAGICIAN) || job_is_a(chr->job, JOB_BLAZE_WIZARD) ? 20 : 10));

        client->stats |= STAT_MAX_HP;
        client->stats |= STAT_MAX_MP;

        character_set_hp(chr, character_get_effective_hp(chr));
        character_set_mp(chr, character_get_effective_mp(chr));
        client->stats |= STAT_HP;
        client->stats |= STAT_MP;

        {
            uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
            union StatValue value = {
                .u8 = client->character.level,
            };
            size_t size = stat_change_packet(true, STAT_LEVEL, &value, packet);
            session_write(client->session, size, packet);
        }

        {
            uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
            show_foreign_effect_packet(client->character.id, 0, packet);
            session_broadcast_to_room(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
        }

    } while (true);
}

void client_gain_meso(struct Client *client, int32_t mesos, bool pickup, bool reward)
{
    struct Character *chr = &client->character;
    if (mesos > 0) {
        if (chr->mesos > INT32_MAX - mesos)
            mesos = INT32_MAX - chr->mesos;
    } else {
        if (chr->mesos + mesos < 0)
            mesos = chr->mesos;
    }

    character_set_meso(chr, chr->mesos + mesos);
    client->stats |= STAT_MESO;

    if (pickup) {
        uint8_t packet[MESO_GAIN_PACKET_LENGTH];
        meso_gain_packet(mesos, packet);
        session_write(client->session, MESO_GAIN_PACKET_LENGTH, packet);
    } else if (reward) {
        uint8_t packet[MESO_GAIN_IN_CHAT_PACKET_LENGTH];
        meso_gain_in_chat_packet(mesos, packet);
        session_write(client->session, MESO_GAIN_IN_CHAT_PACKET_LENGTH, packet);
    }
}

void client_adjust_fame(struct Client *client, int16_t fame)
{
    if (fame > 0 && client->character.fame > INT16_MAX - fame)
        fame = INT16_MAX - client->character.fame;

    if (fame < 0 && client->character.fame < INT16_MIN - fame)
        fame = INT16_MIN - client->character.fame;

    character_set_fame(&client->character, client->character.fame + fame);
    client->stats |= STAT_FAME;
}

bool client_has_item(struct Client *client, uint32_t id, int16_t qty)
{
    uint8_t inv = id / 1000000;
    if (inv == 1) {
        for (uint8_t i = 0; i < client->character.equipmentInventory.slotCount; i++) {
            if (!client->character.equipmentInventory.items[i].isEmpty && client->character.equipmentInventory.items[i].equip.item.itemId == id) {
                qty--;

                if (qty == 0)
                    return true;
            }
        }
    } else {
        inv -= 2;
        for (uint8_t i = 0; i < client->character.inventory[inv].slotCount; i++) {
            if (!client->character.inventory[inv].items[i].isEmpty && client->character.inventory[inv].items[i].item.item.itemId == id) {
                qty -= client->character.inventory[inv].items[i].item.quantity;

                if (qty <= 0)
                    return true;
            }
        }
    }

    return false;
}

bool client_gain_items(struct Client *client, size_t len, const uint32_t *ids, const int16_t *counts, bool reward, bool *success)
{
    // Copy the character's inventories, start trying to insert the items, if we successfuly inserted all items copy the inventories back
    struct Character *chr = &client->character;
    struct InventoryModify *mods = malloc(len * sizeof(struct InventoryModify));
    if (mods == NULL)
        return false;

    size_t mod_capacity = len;
    size_t mod_count = 0;

    int16_t amounts[len];
    for (size_t i = 0; i < len; i++)
        amounts[i] = counts[i];

    struct Inventory invs[4];
    struct {
        uint8_t slotCount;
        struct {
            bool isEmpty;
            struct Equipment equip;
        } items[MAX_ITEM_COUNT];
    } equip_inv;

    for (uint8_t i = 0; i < 4; i++)
        invs[i] = chr->inventory[i];

    equip_inv.slotCount = chr->equipmentInventory.slotCount;
    for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        equip_inv.items[i].isEmpty = chr->equipmentInventory.items[i].isEmpty;
        equip_inv.items[i].equip = chr->equipmentInventory.items[i].equip;
    }

    uint8_t active_projectile = chr->activeProjectile;

    // Try to remove items before adding other ones
    *success = false;
    for (size_t i = 0; i < len; i++) {
        if (amounts[i] < 0) {
            uint8_t inv = ids[i] / 1000000;
            if (inv == 1) {
                for (size_t j = 0; j < equip_inv.slotCount; j++) {
                    if (!equip_inv.items[j].isEmpty && equip_inv.items[j].equip.item.itemId == ids[i]) {
                        equip_inv.items[j].isEmpty = true;
                        amounts[i]++;
                        if (mod_count == mod_capacity) {
                            mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                            mod_capacity *= 2;
                        }

                        mods[mod_count].mode = INVENTORY_MODIFY_TYPE_REMOVE;
                        mods[mod_count].inventory = 1;
                        mods[mod_count].slot = j + 1;
                        mod_count++;
                        if (amounts[i] == 0)
                            break;
                    }
                }

                if (amounts[i] < 0)
                    return true;
            } else {
                inv -= 2;
                for (size_t j = 0; j < invs[inv].slotCount; j++) {
                    if (!invs[inv].items[j].isEmpty && invs[inv].items[j].item.item.itemId == ids[i]) {
                        if (invs[inv].items[j].item.quantity + amounts[i] > 0) {
                            invs[inv].items[j].item.quantity += amounts[i];
                            amounts[i] = 0;
                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mods[mod_count].quantity = invs[inv].items[j].item.quantity;
                            mod_count++;
                        } else {
                            amounts[i] += invs[inv].items[j].item.quantity;
                            invs[inv].items[j].isEmpty = true;
                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_REMOVE;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mod_count++;
                        }
                    }

                    if (amounts[i] == 0)
                        break;
                }
            }
        }
    }

    // Now try adding items
    for (size_t i = 0; i < len; i++) {
        if (amounts[i] > 0) {
            uint8_t inv = ids[i] / 1000000;
            if (inv == 1) {
                for (size_t j = 0; j < equip_inv.slotCount; j++) {
                    if (equip_inv.items[j].isEmpty) {
                        equip_inv.items[j].isEmpty = false;
                        equip_inv.items[j].equip = equipment_from_info(wz_get_equip_info(ids[i]));

                        amounts[i]--;
                        if (mod_count == mod_capacity) {
                            mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                            mod_capacity *= 2;
                        }

                        mods[mod_count].mode = INVENTORY_MODIFY_TYPE_ADD;
                        mods[mod_count].inventory = 1;
                        mods[mod_count].slot = j + 1;
                        mods[mod_count].equip = equip_inv.items[j].equip;
                        mod_count++;
                        if (amounts[i] == 0)
                            break;
                    }
                }

                if (amounts[i] > 0) {
                    free(mods);
                    return true;
                }
            } else {
                inv -= 2;
                // First try to fill up existing stacks
                // Rechargeable items can only fill up empty slots, not existing stacks
                if (!ITEM_IS_RECHARGEABLE(ids[i])) {
                    for (size_t j = 0; j < invs[inv].slotCount; j++) {
                        if (!invs[inv].items[j].isEmpty && invs[inv].items[j].item.item.itemId == ids[i]) {
                            if (invs[inv].items[j].item.quantity > wz_get_item_info(ids[i])->slotMax - amounts[i]) {
                                amounts[i] -= wz_get_item_info(ids[i])->slotMax - invs[inv].items[j].item.quantity;
                                invs[inv].items[j].item.quantity = wz_get_item_info(ids[i])->slotMax;
                            } else {
                                invs[inv].items[j].item.quantity += amounts[i];
                                amounts[i] = 0;
                            }

                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mods[mod_count].quantity = invs[inv].items[j].item.quantity;
                            mod_count++;
                        }

                        if (amounts[i] == 0)
                            break;
                    }
                }

                // Then fill up empty slots
                if (amounts[i] != 0) {
                    for (size_t j = 0; j < invs[inv].slotCount; j++) {
                        if (invs[inv].items[j].isEmpty) {
                            invs[inv].items[j].isEmpty = false;
                            invs[inv].items[j].item.item.id = 0;
                            invs[inv].items[j].item.item.itemId = ids[i];
                            invs[inv].items[j].item.item.flags = 0;
                            invs[inv].items[j].item.item.ownerLength = 0;
                            invs[inv].items[j].item.item.giftFromLength = 0;
                            if (amounts[i] > wz_get_item_info(ids[i])->slotMax) {
                                invs[inv].items[j].item.quantity = wz_get_item_info(ids[i])->slotMax;
                                amounts[i] -= wz_get_item_info(ids[i])->slotMax;
                            } else {
                                invs[inv].items[j].item.quantity = amounts[i];
                                amounts[i] = 0;
                            }

                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_ADD;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mods[mod_count].item = invs[inv].items[j].item;
                            mod_count++;

                            if (inv == 0 && j < active_projectile) {
                                uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
                                if (wid / 10000 == 145 && invs[0].items[j].item.item.itemId / 1000 == 2060) {
                                    // Bow
                                    active_projectile = j;
                                } else if (wid / 10000 == 146 && invs[0].items[j].item.item.itemId / 1000 == 2061) {
                                    // Crossbow
                                    active_projectile = j;
                                } else if (wid / 10000 == 147 && ITEM_IS_THROWING_STAR(invs[0].items[j].item.item.itemId)) {
                                    // Claw
                                    active_projectile = j;
                                } else if (wid / 10000 == 149 && ITEM_IS_BULLET(invs[0].items[j].item.item.itemId)) {
                                    // Gun
                                    active_projectile = j;
                                }
                            }
                        }

                        if (amounts[i] == 0)
                            break;
                    }

                    if (amounts[i] > 0) {
                        free(mods);
                        return true;
                    }
                }
            }
        }
    }

    for (uint8_t i = 0; i < 4; i++)
        chr->inventory[i] = invs[i];

    for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        chr->equipmentInventory.items[i].isEmpty = equip_inv.items[i].isEmpty;
        chr->equipmentInventory.items[i].equip = equip_inv.items[i].equip;
    }

    chr->activeProjectile = active_projectile;

    assert(mod_count != 0);
    while (mod_count > 255) {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(255, mods, packet);
        session_write(client->session, len, packet);
        mod_count -= 255;
    }

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(mod_count, mods, packet);
        session_write(client->session, len, packet);
    }

    if (reward) {
        for (size_t i = 0; i < len; i++) {
            uint8_t packet[ITEM_GAIN_IN_CHAT_PACKET_LENGTH];
            item_gain_in_chat_packet(ids[i], counts[i], packet);
            session_write(client->session, ITEM_GAIN_IN_CHAT_PACKET_LENGTH, packet);
        }
    }

    free(mods);
    *success = true;
    return true;
}

struct CheckQuestItemContext {
    uint32_t id;
    int16_t max;
};

static void check_quest_item(void *data, void *ctx_)
{
    struct CheckQuestItemContext *ctx = ctx_;
    struct Quest *quest = data;

    const struct QuestInfo *info = wz_get_quest_info(quest->id);
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_ITEM &&
                info->endRequirements[i].item.id == ctx->id) {
            ctx->max = info->endRequirements[i].item.count;
            break;
        }
    }

}

bool client_gain_inventory_item(struct Client *client, const struct InventoryItem *item, enum InventoryGainResult *result)
{
    struct Character *chr = &client->character;

    const struct ItemInfo *info = wz_get_item_info(item->item.itemId);

    if (info->quest && hash_set_u32_get(chr->itemQuests, item->item.itemId) == NULL) {
        *result = INVENTORY_GAIN_RESULT_UNAVAILABLE;
        return true;
    }

    struct InventoryModify *mods = malloc(sizeof(struct InventoryModify));
    if (mods == NULL)
        return false;

    size_t mod_capacity = 1;
    size_t mod_count = 0;

    struct Inventory inv;
    inv = chr->inventory[item->item.itemId / 1000000 - 2];

    if (ITEM_IS_RECHARGEABLE(item->item.itemId)) {
        // Rechargeables don't stack like other items
        for (size_t i = 0; i < inv.slotCount; i++) {
            if (inv.items[i].isEmpty) {
                inv.items[i].isEmpty = false;
                inv.items[i].item = *item;

                mods[0].mode = INVENTORY_MODIFY_TYPE_ADD;
                mods[0].inventory = 2;
                mods[0].slot = i + 1;
                mods[0].item = *item;
                mod_count++;

                uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
                if (!chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].isEmpty && i < chr->activeProjectile) {
                    if (wid / 10000 == 147 && ITEM_IS_THROWING_STAR(item->item.itemId) && item->quantity > 0) {
                        // Claw
                        chr->activeProjectile = i;
                    } else if (wid / 10000 == 149 && ITEM_IS_BULLET(item->item.itemId) && item->quantity > 0) {
                        // Gun
                        chr->activeProjectile = i;
                    }
                }

                break;
            }
        }

    } else {
        int16_t quantity = item->quantity;

        // First try to fill up existing stacks
        for (size_t i = 0; i < inv.slotCount; i++) {
            if (!inv.items[i].isEmpty && inv.items[i].item.item.itemId == item->item.itemId) {
                struct CheckQuestItemContext ctx = {
                    .id = item->item.itemId
                };
                hash_set_u16_foreach(chr->quests, check_quest_item, &ctx);
                if (info->quest && inv.items[i].item.quantity + item->quantity == ctx.max) {
                    hash_set_u32_remove(chr->itemQuests, item->item.itemId);
                }

                if (inv.items[i].item.quantity > wz_get_item_info(item->item.itemId)->slotMax - quantity) {
                    quantity -= wz_get_item_info(item->item.itemId)->slotMax - inv.items[i].item.quantity;
                    inv.items[i].item.quantity = wz_get_item_info(item->item.itemId)->slotMax;
                } else {
                    if (inv.items[i].item.item.id == 0) {
                        inv.items[i].item.item.id = item->item.id;
                    }
                    inv.items[i].item.quantity += quantity;
                    quantity = 0;
                }

                if (mod_count == mod_capacity) {
                    void *temp = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                    if (temp == NULL) {
                        free(mods);
                        return false;
                    }

                    mods = temp;
                    mod_capacity *= 2;
                }

                mods[mod_count].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                mods[mod_count].inventory = item->item.itemId / 1000000;
                mods[mod_count].slot = i + 1;
                mods[mod_count].quantity = inv.items[i].item.quantity;
                mod_count++;
            }

            if (quantity == 0)
                break;
        }

        // Then fill up *an* empty slot
        if (quantity != 0) {
            uint8_t j;
            for (j = 0; j < inv.slotCount; j++) {
                if (inv.items[j].isEmpty) {
                    struct CheckQuestItemContext ctx = {
                        .id = item->item.itemId
                    };
                    hash_set_u16_foreach(chr->quests, check_quest_item, &ctx);
                    if (info->quest && item->quantity == ctx.max)
                        hash_set_u32_remove(chr->itemQuests, item->item.itemId);

                    inv.items[j].isEmpty = false;
                    inv.items[j].item.item = item->item;
                    inv.items[j].item.quantity = quantity;

                    if (mod_count == mod_capacity) {
                        void *temp = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                        if (temp == NULL) {
                            free(mods);
                            return false;
                        }

                        mods = temp;
                        mod_capacity *= 2;
                    }

                    // Do it after the possiblity of failure from realloc()
                    // This should still work even when chr->activeProjectile == -1
                    if (!chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].isEmpty && j < chr->activeProjectile) {
                        uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
                        if (wid / 10000 == 145 && item->item.itemId / 1000 == 2060) {
                            // Bow
                            chr->activeProjectile = j;
                        } else if (wid / 10000 == 146 && item->item.itemId / 1000 == 2061) {
                            // Crossbow
                            chr->activeProjectile = j;
                        }
                    }

                    mods[mod_count].mode = INVENTORY_MODIFY_TYPE_ADD;
                    mods[mod_count].inventory = item->item.itemId / 1000000;
                    mods[mod_count].slot = j + 1;
                    mods[mod_count].item = inv.items[j].item;
                    mod_count++;

                    break;
                }
            }

            // Inventory is full
            if (j == inv.slotCount) {
                {
                    uint8_t packet[INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH];
                    inventory_full_notification_packet(packet);
                    session_write(client_get_session(client), INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH, packet);
                }

                {
                    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
                    size_t len = modify_items_packet(0, NULL, packet);
                    session_write(client_get_session(client), len, packet);
                }
                free(mods);
                *result = INVENTORY_GAIN_RESULT_FULL;
                return true;
            }
        }
    }

    chr->inventory[item->item.itemId / 1000000 - 2] = inv;

    while (mod_count > 255) {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(255, mods, packet);
        session_write(client->session, len, packet);
        mod_count -= 255;
    }

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(mod_count, mods, packet);
        session_write(client->session, len, packet);
    }

    free(mods);
    *result = INVENTORY_GAIN_RESULT_SUCCESS;
    return true;
}

bool client_gain_equipment(struct Client *client, const struct Equipment *item, bool equip, enum InventoryGainResult *result)
{
    struct Character *chr = &client->character;
    const struct ItemInfo *info = wz_get_item_info(item->item.itemId);

    if (info->quest && hash_set_u32_get(chr->itemQuests, item->item.itemId) == NULL) {
        *result = INVENTORY_GAIN_RESULT_UNAVAILABLE;
        return true;
    }

    if (!equip) {
        for (size_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
            if (chr->equipmentInventory.items[j].isEmpty) {
                // TODO: This is assuming that a quest can't have more than one equipment of the same kind as a quest item
                if (info->quest)
                    hash_set_u32_remove(chr->itemQuests, item->item.itemId);

                chr->equipmentInventory.items[j].isEmpty = false;
                chr->equipmentInventory.items[j].equip = *item;

                struct InventoryModify mod = {
                    .mode = INVENTORY_MODIFY_TYPE_ADD,
                    .inventory = 1,
                    .slot = j + 1,
                    .equip = chr->equipmentInventory.items[j].equip
                };

                {
                    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
                    size_t len = modify_items_packet(1, &mod, packet);
                    session_write(client->session, len, packet);
                }

                *result = INVENTORY_GAIN_RESULT_SUCCESS;
                return true;
            }
        }
    }

    // TODO: Handle immediate equip
    *result = INVENTORY_GAIN_RESULT_FULL;
    return true;
}

bool client_remove_item(struct Client *client, uint8_t inv, uint8_t src, int16_t amount, bool *success, struct InventoryItem *item)
{
    struct InventoryItem item_;
    if (item == NULL)
        item = &item_;

    struct Character *chr = &client->character;
    if (inv < 2 || inv > 5)
        return false;

    inv -= 2;
    src--;
    if (src >= chr->inventory[inv].slotCount)
        return false;

    *success = false;
    if (chr->inventory[inv].items[src].isEmpty)
        return true;

    *item = chr->inventory[inv].items[src].item;
    if (ITEM_IS_RECHARGEABLE(chr->inventory[inv].items[src].item.item.itemId)) {
        chr->inventory[inv].items[src].isEmpty = true;
    } else {
        if (chr->inventory[inv].items[src].item.quantity < amount)
            return true;

        item->quantity = amount;
        if (chr->inventory[inv].items[src].item.quantity == amount) {
            chr->inventory[inv].items[src].isEmpty = true;
        } else {
            item->item.id = 0;
            chr->inventory[inv].items[src].item.quantity -= amount;
        }
    }

    // Update the active projectile if we removed the currently active one
    if (inv == 0 && chr->activeProjectile == src && (chr->inventory[0].items[src].isEmpty || chr->inventory[0].items[src].item.quantity == 0)) {
        uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
        if (wid / 10000 == 145) {
            // Bow
            chr->activeProjectile = -1;
            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2060) {
                    chr->activeProjectile = i;
                    break;
                }
            }
        } else if (wid / 10000 == 146) {
            // Crossbow
            chr->activeProjectile = -1;
            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2061) {
                    chr->activeProjectile = i;
                    break;
                }
            }
        } else if (wid / 10000 == 147) {
            // Claw
            chr->activeProjectile = -1;
            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_THROWING_STAR(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                    chr->activeProjectile = i;
                    break;
                }
            }
        } else if (wid / 10000 == 149) {
            // Gun
            chr->activeProjectile = -1;
            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_BULLET(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                    chr->activeProjectile = i;
                    break;
                }
            }
        }
    }

    const struct ItemInfo *item_info = wz_get_item_info(item->item.itemId);
    if (item_info->quest && hash_set_u32_get(chr->itemQuests, item->item.itemId) == NULL) {
        hash_set_u32_insert(chr->itemQuests, &item->item.itemId);
    }

    {
        struct InventoryModify mod;
        mod.inventory = inv + 2;
        mod.slot = src + 1;
        if (chr->inventory[inv].items[src].isEmpty) {
            mod.mode = INVENTORY_MODIFY_TYPE_REMOVE;
        } else {
            mod.mode = INVENTORY_MODIFY_TYPE_MODIFY;
            mod.quantity = chr->inventory[inv].items[src].item.quantity;
        }

        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    *success = true;
    return true;
}

bool client_use_projectile(struct Client *client, int16_t amount, bool *success)
{
    struct Character *chr = &client->character;

    *success = false;
    if (chr->activeProjectile == (uint8_t)-1)
        return true;

    uint8_t slot;
    for (slot = chr->activeProjectile; slot < chr->inventory[0].slotCount; slot++) {
        if (chr->inventory[0].items[slot].item.quantity >= amount) {
            if (ITEM_IS_RECHARGEABLE(chr->inventory[0].items[slot].item.item.itemId)) {
                chr->inventory[0].items[slot].item.quantity -= amount;
            } else {
                if (chr->inventory[0].items[slot].item.quantity == amount) {
                    chr->inventory[0].items[slot].isEmpty = true;
                } else {
                    chr->inventory[0].items[slot].item.quantity -= amount;
                }
            }

            break;
        }
    }

    if (slot == chr->inventory[0].slotCount)
        return true;

    // Update the active projectile
    if ((chr->inventory[0].items[chr->activeProjectile].isEmpty || chr->inventory[0].items[chr->activeProjectile].item.quantity == 0)) {
        uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
        if (wid / 10000 == 145) {
            // Bow
            uint8_t i;
            for (i = chr->activeProjectile + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2060) {
                    chr->activeProjectile = i;
                    break;
                }
            }

            if (i == chr->inventory[0].slotCount)
                chr->activeProjectile = -1;
        } else if (wid / 10000 == 146) {
            // Crossbow
            uint8_t i;
            for (i = chr->activeProjectile + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2061) {
                    chr->activeProjectile = i;
                    break;
                }
            }

            if (i == chr->inventory[0].slotCount)
                chr->activeProjectile = -1;
        } else if (wid / 10000 == 147) {
            // Claw
            uint8_t i;
            for (i = chr->activeProjectile + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_THROWING_STAR(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                    chr->activeProjectile = i;
                    break;
                }
            }

            if (i == chr->inventory[0].slotCount)
                chr->activeProjectile = -1;
        } else if (wid / 10000 == 149) {
            // Gun
            uint8_t i;
            for (i = chr->activeProjectile + 1; i < chr->inventory[0].slotCount; i++) {
                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_BULLET(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                    chr->activeProjectile = i;
                    break;
                }
            }

            if (i == chr->inventory[0].slotCount)
                chr->activeProjectile = -1;
        }
    }

    // As far as I know, there are no projectiles that are also quest items
    /*const struct ItemInfo *item_info = wz_get_item_info(item->item.itemId);
    if (item_info->quest && hash_set_u32_get(chr->itemQuests, item->item.itemId) == NULL) {
        hash_set_u32_insert(chr->itemQuests, &item->item.itemId);
    }*/

    {
        struct InventoryModify mod;
        mod.inventory = 2;
        mod.slot = slot + 1;
        if (chr->inventory[0].items[slot].isEmpty) {
            mod.mode = INVENTORY_MODIFY_TYPE_REMOVE;
        } else {
            mod.mode = INVENTORY_MODIFY_TYPE_MODIFY;
            mod.quantity = chr->inventory[0].items[slot].item.quantity;
        }

        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    *success = true;
    return true;

}

bool client_remove_equip(struct Client *client, bool equipped, uint8_t src, bool *success, struct Equipment *equip)
{
    struct Character *chr = &client->character;

    if (equipped) {
        src = equip_slot_to_compact(src);
        if (src >= EQUIP_SLOT_COUNT)
            return false;

        *success = false;
        if (chr->equippedEquipment[src].isEmpty)
            return true;

        *equip = chr->equippedEquipment[src].equip;
        chr->equippedEquipment[src].isEmpty = true;

        {
            struct InventoryModify mod;
            mod.inventory = 1;
            mod.slot = -equip_slot_from_compact(src);
            mod.mode = INVENTORY_MODIFY_TYPE_REMOVE;
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(1, &mod, packet);
            session_write(client->session, len, packet);
        }
    } else {
        src--;
        if (src >= chr->equipmentInventory.slotCount)
            return false;

        *success = false;
        if (chr->equipmentInventory.items[src].isEmpty)
            return true;

        *equip = chr->equipmentInventory.items[src].equip;
        chr->equipmentInventory.items[src].isEmpty = true;

        {
            struct InventoryModify mod;
            mod.inventory = 1;
            mod.slot = src + 1;
            mod.mode = INVENTORY_MODIFY_TYPE_REMOVE;
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(1, &mod, packet);
            session_write(client->session, len, packet);
        }
    }

    *success = true;
    return true;
}

bool client_move_item(struct Client *client, uint8_t inventory, uint8_t src, uint8_t dst)
{
    struct Character *chr = &client->character;
    if (client->shop != -1)
        return true;

    if (inventory < 1 || inventory > 5)
        return false;

    src--;
    dst--;
    if (inventory != 1) {
        inventory -= 2;

        if (src >= chr->inventory[inventory].slotCount || dst >= chr->inventory[inventory].slotCount)
            return false;

        if (chr->inventory[inventory].items[src].isEmpty)
            return true;

        uint8_t mod_count;
        struct InventoryModify mods[2];
        if (chr->inventory[inventory].items[dst].isEmpty) {
            // Destination is empty - move the whole stack there
            chr->inventory[inventory].items[dst] = chr->inventory[inventory].items[src];
            chr->inventory[inventory].items[src].isEmpty = true;
            if (inventory == 0 && src == chr->activeProjectile) {
                if (dst < src) {
                    chr->activeProjectile = dst;
                } else {
                    uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
                    if (wid / 10000 == 145) {
                        // Bow
                        for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                            if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2060) {
                                chr->activeProjectile = i;
                                break;
                            }
                        }
                    } else if (wid / 10000 == 146) {
                        // Crossbow
                        for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                            if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2061) {
                                chr->activeProjectile = i;
                                break;
                            }
                        }
                    } else if (wid / 10000 == 147) {
                        // Claw
                        for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                            struct InventoryItem *item = &chr->inventory[0].items[i].item;
                            if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_THROWING_STAR(item->item.itemId) && item->quantity > 0) {
                                chr->activeProjectile = i;
                                break;
                            }
                        }
                    } else if (wid / 10000 == 149) {
                        // Gun
                        for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                            struct InventoryItem *item = &chr->inventory[0].items[i].item;
                            if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_BULLET(item->item.itemId) && item->quantity > 0) {
                                chr->activeProjectile = i;
                                break;
                            }
                        }
                    }
                }
            } else if (inventory == 0 && dst < chr->activeProjectile) {
                uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
                struct InventoryItem *item = &chr->inventory[0].items[dst].item;
                if (wid / 10000 == 145 && item->item.itemId / 1000 == 2060) {
                    // Bow
                    chr->activeProjectile = dst;
                } else if (wid / 10000 == 146 && item->item.itemId / 1000 == 2061) {
                    chr->activeProjectile = dst;
                } else if (wid / 10000 == 147 && ITEM_IS_THROWING_STAR(item->item.itemId) && item->quantity > 0) {
                    // Claw
                    chr->activeProjectile = dst;
                } else if (wid / 10000 == 149 && ITEM_IS_BULLET(item->item.itemId) && item->quantity > 0) {
                    // Gun
                    chr->activeProjectile = dst;
                }
            }

            mods[0].mode = INVENTORY_MODIFY_TYPE_MOVE;
            mods[0].inventory = inventory + 2;
            mods[0].slot = src + 1;
            mods[0].dst = dst + 1;
            mod_count = 1;
        } else if (chr->inventory[inventory].items[dst].item.item.itemId == chr->inventory[inventory].items[src].item.item.itemId && !ITEM_IS_RECHARGEABLE(chr->inventory[inventory].items[dst].item.item.itemId)) {
            const struct ItemInfo *info = wz_get_item_info(chr->inventory[inventory].items[dst].item.item.itemId);
            if (chr->inventory[inventory].items[dst].item.quantity == info->slotMax) {
                // Destination is full with the same item - swap between the stacks
                // Also note that since this is the same item, the active projectile doesn't need to be updated
                struct InventoryItem temp = chr->inventory[inventory].items[dst].item;
                chr->inventory[inventory].items[dst].item = chr->inventory[inventory].items[src].item;
                chr->inventory[inventory].items[src].item = temp;
                mods[0].mode = INVENTORY_MODIFY_TYPE_MOVE;
                mods[0].inventory = inventory + 2;
                mods[0].slot = src + 1;
                mods[0].dst = dst + 1;
                mod_count = 1;
            } else {
                int16_t amount = info->slotMax - chr->inventory[inventory].items[dst].item.quantity;
                if (chr->inventory[inventory].items[src].item.quantity < amount) {
                    // Destination isn't full and it can consume the whole source - remove the source and modify the destination's item count
                    chr->inventory[inventory].items[dst].item.quantity += chr->inventory[inventory].items[src].item.quantity;
                    chr->inventory[inventory].items[src].isEmpty = true;
                    if (inventory == 0 && src < dst && src == chr->activeProjectile) {
                        if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 145) {
                            // Bow
                            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2060) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 146) {
                            // Crossbow
                            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2061) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 147) {
                            // Claw
                            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_THROWING_STAR(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 149) {
                            // Gun
                            for (uint8_t i = src + 1; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_BULLET(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        }
                    }
                    mods[0].mode = INVENTORY_MODIFY_TYPE_REMOVE;
                    mods[0].inventory = inventory + 2;
                    mods[0].slot = src + 1;
                    mods[1].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                    mods[1].inventory = inventory + 2;
                    mods[1].slot = dst + 1;
                    mods[1].quantity = chr->inventory[inventory].items[dst].item.quantity;
                    mod_count = 2;
                } else {
                    // Destination isn't full and it can't consume the whole source - modify both the source and the destination's item count
                    chr->inventory[inventory].items[dst].item.quantity += amount;
                    chr->inventory[inventory].items[src].item.quantity -= amount;
                    mods[0].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                    mods[0].inventory = inventory + 2;
                    mods[0].slot = src + 1;
                    mods[0].quantity = chr->inventory[inventory].items[src].item.quantity;
                    mods[1].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                    mods[1].inventory = inventory + 2;
                    mods[1].slot = dst + 1;
                    mods[1].quantity = chr->inventory[inventory].items[dst].item.quantity;
                    mod_count = 2;
                }
            }
        } else {
            // Destination has a different item (or it is a rechargeable item) - swap them
            struct InventoryItem temp = chr->inventory[inventory].items[dst].item;
            chr->inventory[inventory].items[dst].item = chr->inventory[inventory].items[src].item;
            chr->inventory[inventory].items[src].item = temp;
            if (inventory == 0) {
                // For easier implementation, make src less then dst
                // There is no need to change the order back afterwards
                if (dst < src) {
                    uint8_t temp = dst;
                    dst = src;
                    src = temp;
                }

                if (src <= chr->activeProjectile && chr->activeProjectile <= dst) {
                    if (chr->activeProjectile == src) {
                        if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 145) {
                            // Bow
                            for (uint8_t i = src; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2060) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 146) {
                            // Crossbow
                            for (uint8_t i = src; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2061) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 147) {
                            // Claw
                            for (uint8_t i = src; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_THROWING_STAR(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        } else if (chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId / 10000 == 149) {
                            // Gun
                            for (uint8_t i = src; i < chr->inventory[0].slotCount; i++) {
                                if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_BULLET(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                                    chr->activeProjectile = i;
                                    break;
                                }
                            }
                        }
                    } else if (chr->activeProjectile == dst) {
                        chr->activeProjectile = src;
                    } else {
                        uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
                        struct InventoryItem *item = &chr->inventory[0].items[src].item;
                        if (wid / 10000 == 145 && item->item.itemId / 1000 == 2060) {
                            // Bow
                            chr->activeProjectile = src;
                        } else if (wid / 10000 == 146 && item->item.itemId / 1000 == 2061) {
                            chr->activeProjectile = src;
                        } else if (wid / 10000 == 147 && ITEM_IS_THROWING_STAR(item->item.itemId) && item->quantity > 0) {
                            // Claw
                            chr->activeProjectile = src;
                        } else if (wid / 10000 == 149 && ITEM_IS_BULLET(item->item.itemId) && item->quantity > 0) {
                            // Gun
                            chr->activeProjectile = src;
                        }

                    }
                }
            }

            mods[0].mode = INVENTORY_MODIFY_TYPE_MOVE;
            mods[0].inventory = inventory + 2;
            mods[0].slot = src + 1;
            mods[0].dst = dst + 1;
            mod_count = 1;
        }

        {
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(mod_count, mods, packet);
            session_write(client->session, len, packet);
        }

        return true;
    } else {
        if (src >= chr->equipmentInventory.slotCount || dst >= chr->equipmentInventory.slotCount)
            return false;

        if (chr->equipmentInventory.items[src].isEmpty)
            return true;

        struct InventoryModify mod;
        if (chr->equipmentInventory.items[dst].isEmpty) {
            chr->equipmentInventory.items[dst] = chr->equipmentInventory.items[src];
            chr->equipmentInventory.items[src].isEmpty = true;
            mod.mode = INVENTORY_MODIFY_TYPE_MOVE;
            mod.inventory = 1;
            mod.slot = src + 1;
            mod.dst = dst + 1;
        } else {
            struct InventoryItem temp = chr->inventory[inventory].items[dst].item;
            chr->inventory[inventory].items[dst].item = chr->inventory[inventory].items[src].item;
            chr->inventory[inventory].items[src].item = temp;
            mod.mode = INVENTORY_MODIFY_TYPE_MOVE;
            mod.inventory = 1;
            mod.slot = src + 1;
            mod.dst = dst + 1;
        }

        {
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(1, &mod, packet);
            session_write(client->session, len, packet);
        }

        return true;
    }
}

bool client_equip(struct Client *client, uint8_t src, enum EquipSlot slot)
{
    // TODO: Check if there is enough room in the equipment inventory when both a top and a bottom are equipped and the player tries to equip an overall
    // or when both a weapon and a shield are equipped and the player tries to equip a two-handed weapon
    struct Character *chr = &client->character;

    if (!is_valid_equip_slot(slot))
        return false;

    uint8_t dst = equip_slot_to_compact(slot);
    src--;
    if (src >= chr->equipmentInventory.slotCount)
        return false;

    if (chr->equipmentInventory.items[src].isEmpty)
        return true;

    if (chr->equippedEquipment[dst].isEmpty) {
        chr->equippedEquipment[dst].isEmpty = false;
        chr->equippedEquipment[dst].equip = chr->equipmentInventory.items[src].equip;

        chr->estr += chr->equippedEquipment[dst].equip.str;
        chr->edex += chr->equippedEquipment[dst].equip.dex;
        chr->eint += chr->equippedEquipment[dst].equip.int_;
        chr->eluk += chr->equippedEquipment[dst].equip.luk;
        chr->eMaxHp += chr->equippedEquipment[dst].equip.hp;
        chr->eMaxMp += chr->equippedEquipment[dst].equip.mp;
        chr->equipmentInventory.items[src].isEmpty = true;
    } else {
        chr->estr -= chr->equippedEquipment[dst].equip.str;
        chr->edex -= chr->equippedEquipment[dst].equip.dex;
        chr->eint -= chr->equippedEquipment[dst].equip.int_;
        chr->eluk -= chr->equippedEquipment[dst].equip.luk;
        chr->eMaxHp -= chr->equippedEquipment[dst].equip.hp;
        chr->eMaxMp -= chr->equippedEquipment[dst].equip.mp;

        chr->estr += chr->equipmentInventory.items[src].equip.str;
        chr->edex += chr->equipmentInventory.items[src].equip.dex;
        chr->eint += chr->equipmentInventory.items[src].equip.int_;
        chr->eluk += chr->equipmentInventory.items[src].equip.luk;
        chr->eMaxHp += chr->equipmentInventory.items[src].equip.hp;
        chr->eMaxMp += chr->equipmentInventory.items[src].equip.mp;

        struct Equipment temp = chr->equipmentInventory.items[src].equip;
        chr->equipmentInventory.items[src].equip = chr->equippedEquipment[dst].equip;
        chr->equippedEquipment[dst].equip = temp;
    }

    if (chr->equippedEquipment[dst].equip.item.itemId / 10000 == 145) {
        // Bow
        for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
            if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2060) {
                chr->activeProjectile = i;
                break;
            }
        }
    } else if (chr->equippedEquipment[dst].equip.item.itemId / 10000 == 146) {
        // Crossbow
        for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
            if (!chr->inventory[0].items[i].isEmpty && chr->inventory[0].items[i].item.item.itemId / 1000 == 2061) {
                chr->activeProjectile = i;
                break;
            }
        }
    } else if (chr->equippedEquipment[dst].equip.item.itemId / 10000 == 147) {
        // Claw
        for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
            if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_THROWING_STAR(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                chr->activeProjectile = i;
                break;
            }
        }
    } else if (chr->equippedEquipment[dst].equip.item.itemId / 10000 == 149) {
        // Gun
        for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
            if (!chr->inventory[0].items[i].isEmpty && ITEM_IS_BULLET(chr->inventory[0].items[i].item.item.itemId) && chr->inventory[0].items[i].item.quantity > 0) {
                chr->activeProjectile = i;
                break;
            }
        }
    }

    struct InventoryModify mod = {
        .mode = INVENTORY_MODIFY_TYPE_MOVE,
        .inventory = 1,
        .slot = src + 1,
        .dst = -slot
    };

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    {
        uint8_t packet[UPDATE_CHAR_LOOK_MAX_LENGTH];
        size_t len = update_char_look_packet(chr, packet);
        session_broadcast_to_room(client->session, len, packet);
    }

    return true;
}

bool client_unequip(struct Client *client, enum EquipSlot slot, uint8_t dst)
{
    // TODO: Also unequip the bottom if the player tries to equip an overall instead of a shirt
    // TODO: Check if all equipment can still be worn after the stat change
    struct Character *chr = &client->character;
    if (!is_valid_equip_slot(slot))
        return false;

    uint8_t src = equip_slot_to_compact(slot);
    dst--;
    if (dst >= chr->equipmentInventory.slotCount)
        return false;

    if (slot == EQUIP_SLOT_WEAPON)
        chr->activeProjectile = -1;

    if (chr->equipmentInventory.items[dst].isEmpty) {
        chr->estr -= chr->equippedEquipment[src].equip.str;
        chr->edex -= chr->equippedEquipment[src].equip.dex;
        chr->eint -= chr->equippedEquipment[src].equip.int_;
        chr->eluk -= chr->equippedEquipment[src].equip.luk;
        chr->eMaxHp -= chr->equippedEquipment[src].equip.hp;
        chr->eMaxMp -= chr->equippedEquipment[src].equip.mp;

        chr->equipmentInventory.items[dst].isEmpty = false;
        chr->equipmentInventory.items[dst].equip = chr->equippedEquipment[src].equip;
        chr->equippedEquipment[src].isEmpty = true;
    } else {
        chr->estr -= chr->equippedEquipment[src].equip.str;
        chr->edex -= chr->equippedEquipment[src].equip.dex;
        chr->eint -= chr->equippedEquipment[src].equip.int_;
        chr->eluk -= chr->equippedEquipment[src].equip.luk;
        chr->eMaxHp -= chr->equippedEquipment[src].equip.hp;
        chr->eMaxMp -= chr->equippedEquipment[src].equip.mp;

        chr->estr += chr->equipmentInventory.items[dst].equip.str;
        chr->edex += chr->equipmentInventory.items[dst].equip.dex;
        chr->eint += chr->equipmentInventory.items[dst].equip.int_;
        chr->eluk += chr->equipmentInventory.items[dst].equip.luk;
        chr->eMaxHp += chr->equipmentInventory.items[dst].equip.hp;
        chr->eMaxMp += chr->equipmentInventory.items[dst].equip.mp;

        struct Equipment temp = chr->equippedEquipment[src].equip;
        chr->equippedEquipment[src].equip = chr->equipmentInventory.items[dst].equip;
        chr->equipmentInventory.items[dst].equip = temp;
    }

    enum Stat stats = 0;
    union StatValue values[2];
    if (chr->hp > character_get_effective_hp(chr)) {
        chr->hp = character_get_effective_hp(chr);
        values[0].i16 = chr->hp;
        stats |= STAT_HP;
    }

    if (chr->mp > character_get_effective_mp(chr)) {
        chr->mp = character_get_effective_mp(chr);
        values[stats != 0 ? 1 : 0].i16 = chr->mp;
        stats |= STAT_MP;
    }

    if (stats != 0) {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, stats, values, packet);
        session_write(client->session, len, packet);
    }

    struct InventoryModify mod = {
        .mode = INVENTORY_MODIFY_TYPE_MOVE,
        .inventory = 1,
        .slot = -slot,
        .dst = dst + 1,
    };

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    {
        uint8_t packet[UPDATE_CHAR_LOOK_MAX_LENGTH];
        size_t len = update_char_look_packet(chr, packet);
        session_broadcast_to_room(client->session, len, packet);
    }

    return true;
}

bool client_use_item(struct Client *client, uint8_t slot, uint32_t id)
{
    struct Character *chr = &client->character;
    slot--;
    if (slot >= chr->inventory[0].slotCount)
        return false;

    // TODO: Check if id is a usable item

    const struct ConsumableInfo *info = wz_get_consumable_info(id);
    if (info == NULL)
        return false;

    if (chr->inventory[0].items[slot].isEmpty || chr->inventory[0].items[slot].item.item.itemId != id)
        return true;

    enum Stat stats = 0;
    size_t value_count = 0;
    union StatValue values[2];

    if (info->hp != 0 || info->hpR != 0) {
        int16_t hp = info->hp + character_get_effective_hp(chr) * info->hpR / 100; // TODO: This addition can overflow
        if (chr->hp > character_get_effective_hp(chr) - hp)
            hp = character_get_effective_hp(chr) - chr->hp;

        chr->hp += hp;

        values[value_count].i16 = chr->hp,
        stats |= STAT_HP;
        value_count++;
    }

    if (info->mp != 0 || info->mpR != 0) {
        int16_t mp = info->mp + character_get_effective_mp(chr) * info->mpR / 100; // TODO: This addition can overflow
        if (chr->mp > character_get_effective_mp(chr) - mp)
            mp = character_get_effective_mp(chr) - chr->mp;

        chr->mp += mp;

        values[value_count].i16 = chr->mp;
        stats |= STAT_MP;
        value_count++;
    }

    // Also enables actions if value_count is 0
    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, stats, values, packet);
        session_write(client->session, len, packet);
    }

    chr->inventory[0].items[slot].item.quantity--;
    if (chr->inventory[0].items[slot].item.quantity == 0) {
        chr->inventory[0].items[slot].isEmpty = true;
        struct InventoryModify mod = {
            .mode = INVENTORY_MODIFY_TYPE_REMOVE,
            .inventory = 2,
            .slot = slot + 1,
        };
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    } else {
        struct InventoryModify mod = {
            .mode = INVENTORY_MODIFY_TYPE_MODIFY,
            .inventory = 2,
            .slot = slot + 1,
            .quantity = chr->inventory[0].items[slot].item.quantity
        };
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    return true;
}

bool client_use_item_immediate(struct Client *client, uint32_t id)
{
    struct Character *chr = &client->character;

    const struct ConsumableInfo *info = wz_get_consumable_info(id);
    if (info == NULL)
        return false;

    //apply_effects(client, info)
    enum Stat stats = 0;
    size_t value_count = 0;
    union StatValue values[2];

    if (info->hp != 0 || info->hpR != 0) {
        int16_t hp = info->hp + character_get_effective_hp(chr) * info->hpR / 100; // TODO: This addition can overflow
        if (chr->hp > character_get_effective_hp(chr) - hp)
            hp = character_get_effective_hp(chr) - chr->hp;

        chr->hp += hp;

        values[value_count].i16 = chr->hp,
        stats |= STAT_HP;
        value_count++;
    }

    if (info->mp != 0 || info->mpR != 0) {
        int16_t mp = info->mp + character_get_effective_mp(chr) * info->mpR / 100; // TODO: This addition can overflow
        if (chr->mp > character_get_effective_mp(chr) - mp)
            mp = character_get_effective_mp(chr) - chr->mp;

        chr->mp += mp;

        values[value_count].i16 = chr->mp;
        stats |= STAT_MP;
        value_count++;
    }

    if (wz_get_item_info(id)->monsterBook) {
        struct MonsterBookEntry *entry = hash_set_u32_get(chr->monsterBook, id);
        if (entry == NULL || entry->count < 5) {
            if (entry == NULL) {
                struct MonsterBookEntry new = {
                    .id = id,
                    .count = 0
                };
                hash_set_u32_insert(chr->monsterBook, &new);
                entry = hash_set_u32_get(chr->monsterBook, id);
            }

            entry->count++;

            {
                uint8_t packet[ADD_CARD_PACKET_LENGTH];
                add_card_packet(false, id, entry->count, packet);
                session_write(client->session, ADD_CARD_PACKET_LENGTH, packet);
            }

            {
                uint8_t packet[SHOW_EFFECT_PACKET_LENGTH];
                show_effect_packet(0x0D, packet);
                session_write(client->session, SHOW_EFFECT_PACKET_LENGTH, packet);
            }
        } else {
            uint8_t packet[ADD_CARD_PACKET_LENGTH];
            add_card_packet(true, id, 5, packet);
            session_write(client->session, ADD_CARD_PACKET_LENGTH, packet);
        }

        // Show monster card effect to other players regardless if the card slot is full or not
        {
            uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
            show_foreign_effect_packet(chr->id, 0x0D, packet);
            session_broadcast_to_room(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
        }
    }

    // Also enables actions if value_count is 0
    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, stats, values, packet);
        session_write(client->session, len, packet);
    }

    return true;
}

bool client_is_quest_started(struct Client *client, uint16_t qid)
{
    return hash_set_u16_get(client->character.quests, qid) != NULL;
}

bool client_is_quest_complete(struct Client *client, uint16_t qid)
{
    return hash_set_u16_get(client->character.completedQuests, qid) != NULL;
}

struct ClientResult client_npc_talk(struct Client *client, uint32_t npc)
{
    if (client->script != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    client->npc = npc;
    char script[12];
    snprintf(script, 12, "%d.lua", npc);
    client->script = script_manager_alloc(client->managers.npc, script, 0);
    if (client->script == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    enum ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    case SCRIPT_RESULT_VALUE_NEXT:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
    break;
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult client_launch_map_script(struct Client *client, const char *script_name)
{
    char script[32];
    snprintf(script, 32, "%s.lua", script_name);
    client->script = script_manager_alloc(client->managers.map, script, 0);

    enum ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    case SCRIPT_RESULT_VALUE_NEXT:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
        break;
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult client_start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted)
{
    struct Character *chr = &client->character;

    if (scripted && client->script != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    const struct QuestInfo *info = wz_get_quest_info(qid);
    if (info == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (hash_set_u16_get(chr->quests, qid) != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    // TODO: Also check if this quest isn't repeatable
    if (hash_set_u16_get(chr->completedQuests, qid) != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if ((scripted && !info->startScript) || (!scripted && info->startScript))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (!check_quest_requirements(chr, info->startRequirementCount, info->startRequirements, npc))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    for (size_t i = 0; i < info->endRequirementCount; i++) {
        if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_ITEM) {
            const struct ItemInfo *item_info = wz_get_item_info(info->endRequirements[i].item.id);
            if (item_info->quest) {
                hash_set_u32_insert(chr->itemQuests, &info->endRequirements[i].item.id);
            }
        }
    }

    if (info->startScript) {
        client->npc = npc;
        client->qid = qid;
        char script[10];
        snprintf(script, 10, "%d.lua", qid);
        client->script = script_manager_alloc(client->managers.quest, script, 0);
        enum ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
        switch (res) {
        case SCRIPT_RESULT_VALUE_KICK:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        case SCRIPT_RESULT_VALUE_FAILURE:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
        case SCRIPT_RESULT_VALUE_SUCCESS:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        case SCRIPT_RESULT_VALUE_NEXT:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
        }
    }

    bool success;
    if (!start_quest(client, qid, npc, &success))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

    if (!success) {
        uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
        char *message = "Please check if you have enough space in your inventory.";
        size_t len = popup_message_packet(strlen(message), message, packet);
        session_write(client->session, len, packet);
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult client_regain_quest_item(struct Client *client, uint16_t qid, uint32_t item_id)
{
    struct Character *chr = &client->character;
    const struct QuestInfo *info = wz_get_quest_info(qid);
    if (info == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (hash_set_u16_get(chr->quests, qid) == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    if (client_has_item(client, item_id, 1))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    size_t i;
    for (i = 0; i < info->startActCount; i++) {
        if (info->startActs[i].type == QUEST_ACT_TYPE_ITEM) {
            size_t j;
            for (j = 0; j < info->startActs[i].item.count; j++) {
                if (info->startActs[i].item.items[j].id == item_id)  {
                    bool success;
                    if (!client_gain_items(client, 1, &item_id, (int16_t[]) { 1 }, true, &success))
                        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

                    if (!success) {
                        uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
                        char *message = "Please check if you have enough space in your inventory.";
                        size_t len = popup_message_packet(strlen(message), message, packet);
                        session_write(client->session, len, packet);
                    }
                    break;
                } else {
                    return (struct ClientResult) {
                        .type = CLIENT_RESULT_TYPE_BAN,
                            .reason = "Client tried to regain an item from a quest that doesn't hand it out"
                    };
                }
            }

            if (j == info->startActs[i].item.count)
                return (struct ClientResult) {
                    .type = CLIENT_RESULT_TYPE_BAN,
                    .reason = "Client tried to regain an item from a quest that doesn't give any quest items"
                };

            break;
        }
    }

    if (i == info->startActCount)
        return (struct ClientResult) {
            .type = CLIENT_RESULT_TYPE_BAN,
            .reason = "Client tried to regain an item from a quest that doesn't have any item acts"
        };

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

const char *client_get_quest_info(struct Client *client, uint16_t info)
{
    struct Character *chr = &client->character;
    struct QuestInfoProgress *qi = hash_set_u16_get(chr->questInfos, info);
    if (qi == NULL)
        return NULL;

    return qi->value;
}

int client_set_quest_info(struct Client *client, uint16_t info, const char *value)
{
    struct Character *chr = &client->character;
    struct QuestInfoProgress *qi = hash_set_u16_get(chr->questInfos, info);
    if (qi == NULL) {
        struct QuestInfoProgress new = {
            .id = info,
            .length = strlen(value)
        };
        strncpy(new.value, value, new.length); // Note that if value is exactly 12 then the nul terminator isn't writter
        if (hash_set_u16_insert(chr->questInfos, &new) == -1)
            return -1;
        uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
        size_t len = update_quest_packet(info, strlen(value), value, packet);
        session_write(client->session, len, packet);
    } else {
        if (strncmp(qi->value, value, qi->length)) {
            uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
            size_t len = update_quest_packet(info, strlen(value), value, packet);
            session_write(client->session, len, packet);
        }

        qi->length = strlen(value);
        strcpy(qi->value, value);
    }

    return 0;
}


bool client_start_quest_now(struct Client *client, bool *success)
{
    return start_quest(client, client->qid, client->npc, success);
}

struct ClientResult client_end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted)
{
    struct Character *chr = &client->character;

    if (scripted && client->script != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    const struct QuestInfo *info = wz_get_quest_info(qid);
    if (info == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (hash_set_u16_get(chr->completedQuests, qid) != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    if (hash_set_u16_get(chr->quests, qid) == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if ((scripted && !info->endScript) || (!scripted && info->endScript))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (!check_quest_requirements(chr, info->endRequirementCount, info->endRequirements, npc))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (info->endScript) {
        client->npc = npc;
        client->qid = qid;
        char script[10];
        snprintf(script, 10, "%d.lua", qid);
        client->script = script_manager_alloc(client->managers.quest, script, 1);
        enum ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
        switch (res) {
        case SCRIPT_RESULT_VALUE_KICK:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        case SCRIPT_RESULT_VALUE_FAILURE:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
        case SCRIPT_RESULT_VALUE_SUCCESS:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        case SCRIPT_RESULT_VALUE_NEXT:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
        }
    }

    bool success;
    if (!end_quest(client, qid, npc, &success))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

    if (!success) {
        uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
        char *message = "Please check if you have enough space in your inventory.";
        size_t len = popup_message_packet(strlen(message), message, packet);
        session_write(client->session, len, packet);
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

bool client_end_quest_now(struct Client *client, bool *success)
{
    return end_quest(client, client->qid, client->npc, success);
}

bool client_forfeit_quest(struct Client *client, uint16_t qid)
{
    struct Quest *quest = hash_set_u16_get(client->character.quests, qid);
    if (quest == NULL)
        return true;

    const struct QuestInfo *info = wz_get_quest_info(qid);
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        struct QuestRequirement *req = &info->endRequirements[i];
        if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
            for (size_t i = 0; i < req->mob.count; i++) {
                if (quest->progress[i] < req->mob.mobs[i].count) {
                    struct MonsterRefCount *m = hash_set_u32_get(client->character.monsterQuests, req->mob.mobs[i].id);
                    m->refCount--;
                    if (m->refCount == 0)
                        hash_set_u32_remove(client->character.monsterQuests, m->id);
                }
            }
        }
    }

    hash_set_u16_remove(client->character.quests, qid);

    {
        uint8_t packet[FORFEIT_QUEST_PACKET_LENGTH];
        forfeit_quest_packet(qid, packet);
        session_write(client->session, FORFEIT_QUEST_PACKET_LENGTH, packet);
    }

    return true;
}

struct ClientResult client_script_cont(struct Client *client, uint8_t prev, uint8_t action, uint32_t selection)
{
    if (client->script == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    // Check if prev matches the actual previous state
    if (client->scriptState != SCRIPT_STATE_WARP) {
        switch (prev) {
        case 0:
            if (client->scriptState != SCRIPT_STATE_OK && client->scriptState != SCRIPT_STATE_NEXT && client->scriptState != SCRIPT_STATE_PREV_NEXT && client->scriptState != SCRIPT_STATE_PREV)
                return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        break;
        case 1:
            if (client->scriptState != SCRIPT_STATE_YES_NO)
                return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        break;
        case 3:
            if (client->scriptState != SCRIPT_STATE_GET_NUMBER)
                return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        break;
        case 4:
            if (client->scriptState != SCRIPT_STATE_SIMPLE)
                return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        break;
        case 12:
            if (client->scriptState != SCRIPT_STATE_ACCEPT_DECILNE)
                return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        break;
        default:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        }
    }

    // In an empty dialogue (GET_NUMBER or SIMPLE), END CHAT is (action == 0) instead of (action == -1)
    bool is_empty_dialoge = client->scriptState == SCRIPT_STATE_GET_NUMBER || client->scriptState == SCRIPT_STATE_SIMPLE;
    if ((is_empty_dialoge && action == 0) || (!is_empty_dialoge && action == (uint8_t)-1)) {
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    }

    // In an empty dialogue, the only other option for action is a '1'
    if (is_empty_dialoge && action != 1) {
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    }

    // Check if 'action' is valid for the current state
    if ((client->scriptState == SCRIPT_STATE_OK && action != 1) ||
            (client->scriptState == SCRIPT_STATE_YES_NO && action != 0 && action != 1) ||
            (client->scriptState == SCRIPT_STATE_NEXT && action != 1) ||
            (client->scriptState == SCRIPT_STATE_PREV_NEXT && action != 0 && action != 1) ||
            (client->scriptState == SCRIPT_STATE_PREV && action != 0 && action != 1) ||
            (client->scriptState == SCRIPT_STATE_ACCEPT_DECILNE && action != 0 && action != 1)) {
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    }

    enum ScriptResult res;
    switch (client->scriptState) {
    case SCRIPT_STATE_OK:
        res = script_manager_run(client->script, 1);
    break;
    case SCRIPT_STATE_YES_NO:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_GET_NUMBER:
    case SCRIPT_STATE_SIMPLE:
        if (action < client->min || action > client->max) {
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
        }
        res = script_manager_run(client->script, selection);
    break;
    case SCRIPT_STATE_NEXT:
        res = script_manager_run(client->script, 1);
    break;
    case SCRIPT_STATE_PREV_NEXT:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_PREV:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_ACCEPT_DECILNE:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_WARP:
        res = script_manager_run(client->script, 0);
    break;
    }

    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    case SCRIPT_RESULT_VALUE_NEXT:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
    }
}

void client_close_script(struct Client *client)
{
    script_manager_free(client->script);
    client->script = NULL;
}

struct CheckProgressContext {
    struct Session *session;
    struct HashSetU32 *monsterQuests;
    uint32_t id;
};

static void check_progress(void *data, void *ctx);

void client_kill_monster(struct Client *client, uint32_t id)
{
    struct Character *chr = &client->character;
    const struct MobInfo *info = wz_get_monster_stats(id);
    client_gain_exp(client, info->exp, false);
    client_commit_stats(client);
    struct CheckProgressContext ctx = {
        .session = client->session,
        .monsterQuests = chr->monsterQuests,
    };

    // I could load Mob.wz/QuestCountGroup and parse the files there
    // but since there are a measly 4 records there it seems better to just
    // make a special case for them here
    if (id == 1110100 || id == 1110130) {
        ctx.id = 9101000;
        if (hash_set_u32_get(client->character.monsterQuests, ctx.id) != NULL)
            hash_set_u16_foreach(chr->quests, check_progress, &ctx);
    } else if (id == 2230101 || id == 2230131) {
        ctx.id = 9101001;
        if (hash_set_u32_get(client->character.monsterQuests, ctx.id) != NULL)
            hash_set_u16_foreach(chr->quests, check_progress, &ctx);
    } else if (id == 1140100 || id == 1140130) {
        ctx.id = 9101002;
        if (hash_set_u32_get(client->character.monsterQuests, ctx.id) != NULL)
            hash_set_u16_foreach(chr->quests, check_progress, &ctx);
    } else if (id == 8830003 || id == 8830010) {
        ctx.id = 9101003;
        if (hash_set_u32_get(client->character.monsterQuests, ctx.id) != NULL)
            hash_set_u16_foreach(chr->quests, check_progress, &ctx);
    }

    ctx.id = id;

    if (hash_set_u32_get(client->character.monsterQuests, id) != NULL)
        hash_set_u16_foreach(chr->quests, check_progress, &ctx);
}

struct ClientResult client_open_shop(struct Client *client, uint32_t id)
{
    if (client->shop == -1) {
        client->shop = id;

        const struct ShopInfo *info = shop_info_find(id);
        if (info == NULL)
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "" };

        struct ShopItem items[info->count];
        for (size_t i = 0; i < info->count; i++) {
            items[i].id = info->items[i].id;
            items[i].price = info->items[i].price;
        }

        uint8_t packet[OPEN_SHOP_PACKET_MAX_LENGTH];
        size_t len = open_shop_packet(id, info->count, items, packet);
        session_write(client->session, len, packet);
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult client_buy(struct Client *client, uint16_t pos, uint32_t id, int16_t quantity, int32_t price)
{
    if (client->shop == -1)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "Client tried to buy from a shop that isn't open" };

    const struct ShopInfo *shop = shop_info_find(client->shop);
    if (pos >= shop->count)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "Client tried to buy an illegal shop position" };

    // Also implicitly checks that the item ID actuallt exists
    if (shop->items[pos].id != id)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "Client tried to buy an item with an incorrect ID" };

    if (quantity <= 0)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "Client tried to buy a non-positive quantity of an item" };

    if (shop->items[pos].id / 1000000 == 1 && quantity > 1)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "Client tried to buy multiple of an equipment" };

    // TODO: Check if price is per item or total
    if (shop->items[pos].price != price)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "Client tried to buy an item with an incorrect price" };

    // Players can drop meso while in the shop, so this can be a legal packet
    if (client->character.mesos < price) {
        uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
        shop_action_response(2, packet);
        session_write(client->session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);

        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    }

    bool success;
    if (!client_gain_items(client, 1, &id, &quantity, false, &success))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

    client_gain_meso(client, -price, false, false);
    client_commit_stats(client);

    if (!success)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN, .reason = "Client tried to buy an item while not having a slot for it" };

    uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
    shop_action_response(0, packet);
    session_write(client->session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult client_sell(struct Client *client, uint16_t pos, uint32_t id, int16_t quantity)
{
    if (quantity <= 0)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    // TODO: Check if item is sellable
    const struct ItemInfo *info = wz_get_item_info(id);
    if (info == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    uint8_t inv = id / 1000000;
    if (inv == 1) {
        if (quantity > 1)
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

        bool success;
        struct Equipment equip;
        if (!client_remove_equip(client, false, pos, &success, &equip))
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

        if (!success || equip.item.itemId != id)
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    } else {
        bool success;
        struct InventoryItem item;
        if (!client_remove_item(client, inv, pos, quantity, &success, &item))
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

        if (!success || item.item.itemId != id)
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    }

    client_gain_meso(client, info->price * quantity, false, false);
    client_commit_stats(client);

    uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
    shop_action_response(0x8, packet);
    session_write(client->session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult client_recharge(struct Client *client, uint16_t slot)
{
    struct Character *chr = &client->character;

    slot--;
    if (slot >= chr->inventory[0].slotCount)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (chr->inventory[0].items[slot].isEmpty)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    if (!ITEM_IS_RECHARGEABLE(chr->inventory[0].items[slot].item.item.itemId))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    const struct ItemInfo *info = wz_get_item_info(chr->inventory[0].items[slot].item.item.itemId);

    if (chr->inventory[0].items[slot].item.quantity >= info->slotMax)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };

    int32_t price = (info->slotMax - chr->inventory[0].items[slot].item.quantity) * info->unitPrice;

    if (price > chr->mesos) {
        uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
        shop_action_response(0x2, packet);
        session_write(client->session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    }

    chr->inventory[0].items[slot].item.quantity = info->slotMax;

    if (slot < chr->activeProjectile) {
        uint32_t wid = chr->equippedEquipment[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].equip.item.itemId;
        // Claw
        if (wid / 10000 == 147 && ITEM_IS_THROWING_STAR(chr->inventory[0].items[slot].item.item.itemId)) {
            chr->activeProjectile = slot;
        // Gun
        } else if (wid / 10000 == 149 && ITEM_IS_BULLET(chr->inventory[0].items[slot].item.item.itemId)) {
            chr->activeProjectile = slot;
        }
    }

    {
        struct InventoryModify mod = {
            .mode = INVENTORY_MODIFY_TYPE_MODIFY,
            .inventory = 2,
            .slot = slot + 1,
            .quantity = info->slotMax
        };

        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    client_gain_meso(client, -price, false, false);
    client_commit_stats(client);

    uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
    shop_action_response(0x8, packet);
    session_write(client->session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

}

bool client_close_shop(struct Client *client)
{
    if (client->shop == -1)
        return false;

    client->shop = -1;
    return true;
}

bool client_is_in_shop(struct Client *client)
{
    return client->shop != -1;
}

void client_send_ok(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker)
{
    client->scriptState = SCRIPT_STATE_OK;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_OK, msg_len, msg, speaker, packet);
    session_write(client->session, len, packet);
}

void client_send_yes_no(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker)
{
    client->scriptState = SCRIPT_STATE_YES_NO;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_YES_NO, msg_len, msg, speaker, packet);
    session_write(client->session, len, packet);
}

void client_send_simple(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker, uint32_t selection_count)
{
    client->scriptState = SCRIPT_STATE_SIMPLE;
    client->min = 1;
    client->max = selection_count;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_SIMPLE, msg_len, msg, speaker, packet);
    session_write(client->session, len, packet);
}

void client_send_next(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker)
{
    client->scriptState = SCRIPT_STATE_NEXT;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_NEXT, msg_len, msg, speaker, packet);
    session_write(client->session, len, packet);
}

void client_send_prev_next(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker)
{
    client->scriptState = SCRIPT_STATE_PREV_NEXT;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_PREV_NEXT, msg_len, msg, speaker, packet);
    session_write(client->session, len, packet);
}

void client_send_prev(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker)
{
    client->scriptState = SCRIPT_STATE_PREV;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_PREV, msg_len, msg, speaker, packet);
    session_write(client->session, len, packet);
}

void client_send_accept_decline(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker)
{
    client->scriptState = SCRIPT_STATE_ACCEPT_DECILNE;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_ACCEPT_DECILNE, msg_len, msg, speaker, packet);
    session_write(client->session, len, packet);
}

void client_send_get_number(struct Client *client, size_t msg_len, const char *msg, uint8_t speaker, int32_t def, int32_t min, int32_t max)
{
    client->scriptState = SCRIPT_STATE_GET_NUMBER;
    client->min = min;
    client->max = max;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_get_number_packet(client->npc, msg_len, msg, speaker, def, min, max, packet);
    session_write(client->session, len, packet);
}

void client_message(struct Client *client, const char *msg)
{
    uint8_t packet[SERVER_MESSAGE_PACKET_MAX_LENGTH];
    size_t len = server_message_packet(strlen(msg), msg, packet);
    session_write(client->session, len, packet);
}

bool client_warp(struct Client *client, uint32_t map, uint8_t portal)
{
    struct Character *chr = &client->character;
    client->scriptState = SCRIPT_STATE_WARP;
    {
        uint8_t packet[REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH];
        remove_player_from_map_packet(chr->id, packet);
        session_broadcast_to_room(client->session, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);
    }

    map_leave(room_get_context(session_get_room(client->session)), client->map.player);
    client->map.player = NULL;

    {
        uint8_t packet[CHANGE_MAP_PACKET_LENGTH];
        change_map_packet(&client->character, map, portal, packet);
        session_write(client->session, CHANGE_MAP_PACKET_LENGTH, packet);
    }

    client->character.map = map;
    client->character.spawnPoint = portal;

    session_change_room(client->session, map);

    return true;
}

void client_reset_stats(struct Client *client)
{
    struct Character *chr = &client->character;
    chr->ap += chr->str + chr->int_ + chr->dex + chr->luk - 16;
    chr->str = 4;
    chr->dex = 4;
    chr->int_ = 4;
    chr->luk = 4;
    chr->sp = 1;

    switch (chr->job) {
        case JOB_SWORDSMAN:
        case JOB_DAWN_WARRIOR:
        case JOB_ARAN:
            chr->ap -= 35 - chr->str;
            chr->str = 35;
            chr->sp += (chr->level - 10) * 3;
        break;

        case JOB_MAGICIAN:
        case JOB_BLAZE_WIZARD:
            chr->ap -= 20 - chr->int_;
            chr->int_ = 20;
            chr->sp += (chr->level - 8) * 3;
        break;

        case JOB_ARCHER:
        case JOB_WIND_ARCHER:
        case JOB_ROGUE:
        case JOB_NIGHT_WALKER:
            chr->ap -= 25 - chr->dex;
            chr->dex = 25;
            chr->sp += (chr->level - 10) * 3;
        break;

        case JOB_PIRATE:
        case JOB_THUNDER_BREAKER:
            chr->ap -= 20 - chr->dex;
            chr->dex = 20;
            chr->sp += (chr->level - 10) * 3;
        break;

        default:
        break;
    }

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue values[] = {
        {
            .i16 = chr->str
        },
        {
            .i16 = chr->dex
        },
        {
            .i16 = chr->int_
        },
        {
            .i16 = chr->luk
        },
        {
            .i16 = chr->ap
        },
        {
            .i16 = chr->sp
        },
    };
    size_t len = stat_change_packet(true, STAT_STR | STAT_DEX | STAT_INT | STAT_LUK | STAT_AP | STAT_SP, values, packet);
    session_write(client->session, len, packet);
}

void client_change_job(struct Client *client, enum Job job)
{
    struct Character *chr = &client->character;

    character_set_job(chr, job);
    client->stats |= STAT_JOB;
    int16_t sp = 1;
    if (job % 10 == 2)
        sp += 2;

    client_adjust_sp(client, sp);

    // TODO: Gain inventory slots

    int16_t rand1 = random();
    if (chr->job % 1000 == 100) {
        rand1 = rand1 % (250 - 200 + 1) + 200;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        character_set_max_hp(chr, chr->maxHp + rand1);
    } else if (chr->job % 1000 == 200) {
        rand1 = rand1 % (150 - 100 + 1) + 100;
        if (chr->maxMp > 30000 - rand1)
            rand1 = 30000 - chr->maxMp;
        character_set_max_mp(chr, chr->maxMp + rand1);
    } else if (chr->job % 100 == 0) {
        rand1 = rand1 % (150 - 100 + 1) + 100;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        character_set_max_hp(chr, chr->maxHp + rand1);

        int16_t rand2 = random() % (50 - 25 + 1) + 25;
        if (chr->maxMp > 30000 - rand2)
            rand2 = 30000 - chr->maxMp;
        character_set_max_mp(chr, chr->maxMp + rand2);
    } else if (chr->job % 1000 > 0 && chr->job % 1000 < 200) {
        rand1 = rand1 % (350 - 300 + 1) + 300;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        character_set_max_hp(chr, chr->maxHp + rand1);
    } else if (chr->job % 1000 < 300) {
        rand1 = rand1 % (500 - 450 + 1) + 450;
        if (chr->maxMp > 30000 - rand1)
            rand1 = 30000 - chr->maxMp;
        character_set_max_mp(chr, chr->maxMp + rand1);
    } else if (chr->job % 1000 > 300) {
        rand1 = rand1 % (350 - 300 + 1) + 300;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        character_set_max_hp(chr, chr->maxHp + rand1);

        int16_t rand2 = random() % (200 - 150 + 1) + 150;
        if (chr->maxMp > 30000 - rand2)
            rand2 = 30000 - chr->maxMp;
        character_set_max_mp(chr, chr->maxMp + rand2);
    }

    client->stats |= STAT_MAX_HP | STAT_MAX_MP;
    client_commit_stats(client);

    // TODO: Broadcast to family, party, guild

    //{
    //    uint8_t packet[REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH];
    //    remove_player_from_map_packet(chr->id, packet);
    //    session_broadcast_to_room(client->session.session, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);
    //}

    //{
    //    uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
    //    size_t len = add_player_to_map_packet(chr, false, packet);
    //    session_broadcast_to_room(client->session.session, len, packet);
    //}

    {
        uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
        show_foreign_effect_packet(chr->id, 8, packet);
        session_broadcast_to_room(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
    }
}

struct ClientResult client_launch_portal_script(struct Client *client, const char *portal)
{
    if (client->script != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    char script[21];
    snprintf(script, 21, "%s.lua", portal);
    client->script = script_manager_alloc(client->managers.portal, script, 0);
    enum ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    case SCRIPT_RESULT_VALUE_NEXT:
    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
        break;
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

void client_enable_actions(struct Client *client)
{
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    size_t len = stat_change_packet(true, 0, NULL, packet);
    session_write(client->session, len, packet);
}

void client_toggle_auto_pickup(struct Client *client)
{
    client->autoPickup = !client->autoPickup;
}

bool client_is_auto_pickup_enabled(struct Client *client)
{
    return client->autoPickup;
}

bool client_apply_skill(struct Client *client, uint32_t skill_id, uint8_t *level)
{
    struct Character *chr = &client->character;
    struct Skill *skill = hash_set_u32_get(chr->skills, skill_id);
    if (skill == NULL)
        return false; // Packet-edit

    const struct SkillInfo *info = wz_get_skill_info(skill_id);
    if (info == NULL)
        return false; // Packet-edit

    if (info->levels[skill->level-1].bulletCount > 0) {
        if (chr->activeProjectile == (uint8_t)-1)
            return true;

        bool success;
        if (!client_use_projectile(client, info->levels[skill->level-1].bulletCount, &success))
            return false;

        if (!success)
            return true;
    }

    client_adjust_mp_now(client, -info->levels[skill->level-1].mpCon);

    *level = skill->level;

    return true;
}

bool client_add_key(struct Client *client, uint32_t key, uint8_t type, uint32_t action)
{
    struct Character *chr = &client->character;

    if (key >= KEYMAP_MAX_KEYS)
        return false;

    chr->keyMap[key].type = type;
    chr->keyMap[key].action = action;
    return true;
}

bool client_add_skill_key(struct Client *client, uint32_t key, uint32_t skill_id)
{
    // TODO: Check if skill is allowed to be bound
    struct Character *chr = &client->character;

    if (key >= KEYMAP_MAX_KEYS)
        return false;

    chr->keyMap[key].type = 1;
    chr->keyMap[key].action = skill_id;
    return true;
}

bool client_remove_key(struct Client *client, uint32_t key, uint32_t action)
{
    struct Character *chr = &client->character;

    if (key >= KEYMAP_MAX_KEYS || chr->keyMap[key].type == 0)
        return false;

    chr->keyMap[key].type = 0;
    chr->keyMap[key].action = 0;
    return true;
}

bool client_sit(struct Client *client, uint32_t id)
{
    struct Character *chr = &client->character;

    if (!ITEM_IS_CHAIR(id))
        return false;

    if (chr->chair != 0 || chr->seat != (uint16_t)-1)
        return false;

    if (!client_has_item(client, id, 1))
        return true;

    chr->chair = id;

    uint8_t packet[SET_CHAIR_PACKET_LENGTH];
    set_chair_packet(chr->id, id, packet);
    room_broadcast(session_get_room(client->session), SET_CHAIR_PACKET_LENGTH, packet);

    return true;
}

bool client_sit_on_map_seat(struct Client *client, uint16_t id)
{
    struct Character *chr = &client->character;
    if (id >= wz_get_map_seat_count(chr->map))
        return false;

    if (chr->seat != (uint16_t)-1)
        return true;

    if (!map_try_occupy_seat(room_get_context(session_get_room(client->session)), id))
        return true;

    chr->seat = id;

    uint8_t packet[SIT_ON_MAP_SEAT_PACKET_LENGTH];
    sit_on_map_seat_packet(id, packet);
    session_write(client->session, SIT_ON_MAP_SEAT_PACKET_LENGTH, packet);

    // Other map players will be notified of the sitting by a client_move()

    return true;
}

bool client_stand_up(struct Client *client)
{
    struct Character *chr = &client->character;

    if (chr->chair != 0) {
        chr->chair = 0;

        {
            uint8_t packet[SET_CHAIR_PACKET_LENGTH];
            set_chair_packet(chr->id, 0, packet);
            room_broadcast(session_get_room(client->session), SET_CHAIR_PACKET_LENGTH, packet);
        }
    } else if (chr->seat != (uint16_t)-1) {
        map_tire_seat(room_get_context(session_get_room(client->session)), chr->seat);
        chr->seat = -1;
    }

    {
        uint8_t packet[STAND_UP_PACKET_LENGTH];
        stand_up_packet(packet);
        session_write(client->session, STAND_UP_PACKET_LENGTH, packet);
    }

    return true;
}

bool client_open_storage(struct Client *client)
{
    uint8_t packet[OPEN_STORAGE_PACKET_MAX_LENGTH];
    size_t len = open_storage_packet(&client->character.storage, client->npc, packet);
    session_write(client->session, len, packet);
    return true;
}

void client_show_info(struct Client *client, const char *path)
{
    uint8_t packet[SHOW_INFO_PACKET_MAX_LENGTH];
    size_t len = show_info_packet(strlen(path), path, packet);
    session_write(client->session, len, packet);
}

void client_show_intro(struct Client *client, const char *path)
{
    uint8_t packet[SHOW_INTRO_PACKET_MAX_LENGTH];
    size_t len = show_intro_packet(strlen(path), path, packet);
    session_write(client->session, len, packet);
}

void client_create_party(struct Client *client)
{
    if (client->party != NULL)
        return;

    client->party = party_create(client->character.id);
    if (client->party == NULL) {
        uint8_t packet[PARTY_STATUS_MESSAGE_PACKET_LENGTH];
        party_status_message_packet(1, packet);
        session_write(client->session, PARTY_STATUS_MESSAGE_PACKET_LENGTH, packet);
    } else {
        uint8_t packet[PARTY_CREATE_PACKET_LENGTH];
        party_create_packet(party_get_id(client->party), packet);
        session_write(client->session, PARTY_CREATE_PACKET_LENGTH, packet);
    }
}

void client_invite_to_party(struct Client *client, uint8_t name_len, const char *name)
{
    if (client->party == NULL)
        return;

    struct ClientCommand *cmd = malloc(sizeof(struct ClientCommand));
    cmd->type = CLIENT_COMMAND_PARTY_INVITE;
    cmd->id = party_get_id(client->party);
    cmd->nameLen = client->character.nameLength;

    memcpy(cmd->name, client->character.name, client->character.nameLength);

    bool sent = session_send_command(client->session, find_id_by_name(name_len, name), cmd);

    if (!sent) {
        free(cmd);
        uint8_t packet[PARTY_STATUS_MESSAGE_PACKET_LENGTH];
        party_status_message_packet(19, packet);
        session_write(client->session, PARTY_STATUS_MESSAGE_PACKET_LENGTH, packet);
    }
}

void client_reject_party_invitaion(struct Client *client, uint8_t name_len, const char *name)
{
    struct ClientCommand *cmd = malloc(sizeof(struct ClientCommand));
    cmd->type = CLIENT_COMMAND_PARTY_INVITE;
    cmd->reason = 0;
    cmd->nameLen = client->character.nameLength;

    memcpy(cmd->name, client->character.name, client->character.nameLength);

    bool sent = session_send_command(client->session, find_id_by_name(name_len, name), cmd);

    if (!sent)
        free(cmd);
}

void client_announce_party_join(struct Client *client, uint32_t id)
{
    //uint8_t packet[PARTY_JOIN_PACKET_MAX_LENGTH];
    //size_t len = party_join_packet(id, packet);
    //session_write(client->session, len, packet);
}

void client_announce_party_leave(struct Client *client, uint32_t id)
{
}

void client_announce_party_kick(struct Client *client, uint32_t id)
{
}

void client_announce_party_disband(struct Client *client)
{
    if (client->party != NULL) {
        uint8_t packet[PARTY_DISBAND_PACKET_LENGTH];
        party_disband_packet(party_get_id(client->party), party_get_leader_id(client->party), packet);
        session_write(client->session, PARTY_DISBAND_PACKET_LENGTH, packet);
    }
}

void client_announce_party_change_online_status(struct Client *client, uint32_t id)
{
}

void client_announce_party_change_leader(struct Client *client, uint32_t id)
{
}

static void insert_client(uint8_t len, const char *name, uint32_t id)
{
    mtx_lock(&CLIENTS_LOCK);
    CLIENTS = realloc(CLIENTS, (CLIENTS_LENGTH + 1) * sizeof(*CLIENTS));
    CLIENTS[CLIENTS_LENGTH].id = id;
    strncpy(CLIENTS[CLIENTS_LENGTH].name, name, len);
    CLIENTS[CLIENTS_LENGTH].len = len;
    CLIENTS_LENGTH++;
    mtx_unlock(&CLIENTS_LOCK);
}

static uint32_t find_id_by_name(uint8_t len, const char *name)
{
    uint32_t ret = 0;
    mtx_lock(&CLIENTS_LOCK);
    for (size_t i = 0; i < CLIENTS_LENGTH; i++) {
        if (len == CLIENTS[i].len && strncmp(CLIENTS[i].name, name, CLIENTS[i].len) == 0) {
            ret = CLIENTS[i].id;
            break;
        }
    }

    mtx_unlock(&CLIENTS_LOCK);
    return ret;
}

static bool check_quest_requirements(struct Character *chr, size_t req_count, const struct QuestRequirement *reqs, uint32_t npc)
{
    for (size_t i = 0; i < req_count; i++) {
        switch (reqs[i].type) {
        case QUEST_REQUIREMENT_TYPE_NPC:
            if (reqs[i].npc.id != npc)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_QUEST:
            switch (reqs[i].quest.state) {
            case QUEST_STATE_NOT_STARTED:
                if (hash_set_u16_get(chr->quests, reqs[i].quest.id) != NULL)
                    return false;

                if (hash_set_u16_get(chr->completedQuests, reqs[i].quest.id) != NULL)
                    return false;
            break;

            case QUEST_STATE_STARTED:
                if (hash_set_u16_get(chr->quests, reqs[i].quest.id) == NULL)
                    return false;
            break;

            case QUEST_STATE_COMPLETED:
                if (hash_set_u16_get(chr->completedQuests, reqs[i].quest.id) == NULL)
                    return false;
            break;
            }
        break;

        case QUEST_REQUIREMENT_TYPE_JOB: {
            size_t j;
            for (j = 0; j < reqs[i].job.count; j++) {
                if (reqs[i].job.jobs[j] == chr->job)
                    break;
            }

            if (j == reqs[i].job.count)
                return false;
        }
        break;

        case QUEST_REQUIREMENT_TYPE_COMPLETED_QUEST:
            if (hash_set_u16_size(chr->completedQuests) < reqs[i].questCompleted.amount)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_MESO:
            if (chr->mesos < reqs[i].meso.amount)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_MIN_LEVEL:
            if (chr->level < reqs[i].minLevel.level)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_MAX_LEVEL:
            if (chr->level > reqs[i].maxLevel.level)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_ITEM: {
            enum InventoryType inv = item_get_inventory(reqs[i].item.id);
            if (reqs[i].item.count != 0) {
                int32_t amount = 0;
                if (inv == INVENTORY_TYPE_EQUIP) {
                    for (size_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
                        if (!chr->equipmentInventory.items[j].isEmpty && chr->equipmentInventory.items[j].equip.item.itemId == reqs[i].item.id)
                            amount++;
                    }

                    // TODO: Check in the equipped equipment

                    if (amount < reqs[i].item.count)
                        return false;
                } else {
                    for (size_t j = 0; j < chr->inventory[inv - 1].slotCount; j++) {
                        if (!chr->inventory[inv - 1].items[j].isEmpty && chr->inventory[inv - 1].items[j].item.item.itemId == reqs[i].item.id)
                            amount += chr->inventory[inv - 1].items[j].item.quantity;
                    }

                    if (amount < reqs[i].item.count)
                        return false;
                }
            } else {
                // if count is 0 this means that the player shouldn't have any of the item
                if (inv == INVENTORY_TYPE_EQUIP) {
                    for (size_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
                        if (!chr->equipmentInventory.items[j].isEmpty && chr->equipmentInventory.items[j].equip.item.itemId == reqs[i].item.id)
                            return false;
                    }
                } else {
                    for (size_t j = 0; j < chr->inventory[inv - 1].slotCount; j++) {
                        if (!chr->inventory[inv - 1].items[j].isEmpty && chr->inventory[inv - 1].items[j].item.item.itemId == reqs[i].item.id)
                            return false;
                    }
                }

            }
        }
        break;

        case QUEST_REQUIREMENT_TYPE_INFO: {
            struct QuestInfoProgress *info = hash_set_u16_get(chr->questInfos, reqs[i].info.number);
            if (info == NULL)
                return false;

            // TODO: For now, only implement single quest-info value
            if (strcmp(info->value, reqs[i].info.infos[0]))
                return false;
        }
        break;

        default: {
            fprintf(stderr, "Unimplemented\n");
        }
        }
    }

    return true;
}

static bool start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success)
{
    bool success_;
    if (success == NULL)
        success = &success_;

    struct Character *chr = &client->character;

    const struct QuestInfo *info = wz_get_quest_info(qid);

    struct Quest quest = {
        .id = qid,
        .progressCount = 0
    };

    for (size_t i = 0; i < info->endRequirementCount; i++) {
        struct QuestRequirement *req = &info->endRequirements[i];
        if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
            for (size_t i = 0; i < req->mob.count; i++) {
                struct MonsterRefCount *m = hash_set_u32_get(chr->monsterQuests, req->mob.mobs[i].id);
                if (m == NULL) {
                    struct MonsterRefCount new = {
                        .id = req->mob.mobs[i].id,
                        .refCount = 1
                    };
                    hash_set_u32_insert(chr->monsterQuests, &new);
                } else {
                    m->refCount++;
                }
                quest.progress[quest.progressCount] = 0;
                quest.progressCount++;
            }
        }
    }

    if (hash_set_u16_insert(chr->quests, &quest) == -1)
        return false;

    *success = false;
    for (size_t i = 0; i < info->startActCount; i++) {
        switch (info->startActs[i].type) {
        case QUEST_ACT_TYPE_EXP:
            client_gain_exp(client, info->startActs[i].exp.amount, true);
        break;
        case QUEST_ACT_TYPE_MESO:
            client_gain_meso(client, info->startActs[i].meso.amount, false, true);
        break;
        case QUEST_ACT_TYPE_ITEM: {
            bool has_prop = false;
            int8_t item_count = 0;
            int32_t total = 0;
            for (size_t j = 0; j < info->startActs[i].item.count; j++) {
                if (info->startActs[i].item.items[j].prop == 0 || !has_prop) {
                    if (info->startActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    item_count++;
                }

                total += info->startActs[i].item.items[j].prop;
            }

            uint32_t ids[item_count];
            int16_t amounts[item_count];

            int32_t r;
            if (has_prop) {
                // If there is a random item, there should be at least one empty slot in each inventory
                size_t j;
                for (j = 0; j < chr->equipmentInventory.slotCount; j++)
                    if (chr->equipmentInventory.items[j].isEmpty)
                        break;

                if (j == chr->equipmentInventory.slotCount) {
                    hash_set_u16_remove(chr->quests, qid);
                    return true;
                }

                for (size_t i = 0; i < 4; i++) {
                    for (j = 0; j < chr->inventory[i].slotCount; j++) {
                        if (chr->inventory[i].items[j].isEmpty)
                            break;
                    }

                    if (j == chr->inventory[i].slotCount) {
                        hash_set_u16_remove(chr->quests, qid);
                        return true;
                    }
                }

                r = rand() % total;
            }

            has_prop = false;
            item_count = 0;
            total = 0;
            for (size_t j = 0; j < info->startActs[i].item.count; j++) {
                total += info->startActs[i].item.items[j].prop;
                if (info->startActs[i].item.items[j].prop == 0 || (!has_prop && r < total)) {
                    if (info->startActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    ids[item_count] = info->startActs[i].item.items[j].id;
                    amounts[item_count] = info->startActs[i].item.items[j].count;
                    item_count++;
                }
            }

            for (int8_t i = 0; i < item_count; i++) {
                if (client_has_item(client, ids[i], 1)) {
                    item_count--;
                    ids[i] = ids[item_count];
                    amounts[i] = amounts[item_count];
                    i--;
                }
            }

            bool success;
            if (!client_gain_items(client, item_count, ids, amounts, true, &success)) {
                hash_set_u16_remove(chr->quests, qid);
                return false;
            }

            if (!success) {
                hash_set_u16_remove(chr->quests, qid);
                return true;
            }
        }
        break;

        default:
            fprintf(stderr, "Unimplemented\n");
        }
    }

    // No need to enable actions if no stat has changed
    if (client->stats != 0)
        client_commit_stats(client);

    char progress[15];
    size_t prog_len = quest_get_progress_string(&quest, progress);
    {
        uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
        size_t len = update_quest_packet(qid, prog_len, progress, packet);
        session_write(client->session, len, packet);
    }

    {
        uint8_t packet[START_QUEST_PACKET_LENGTH];
        start_quest_packet(qid, npc, packet);
        session_write(client->session, START_QUEST_PACKET_LENGTH, packet);
    }

    *success = true;
    return true;
}

static bool end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success)
{
    bool success_;
    if (success == NULL)
        success = &success_;

    struct Character *chr = &client->character;

    struct CompletedQuest quest = {
        .id = qid,
        .time = time(NULL),
    };

    if (hash_set_u16_insert(chr->completedQuests, &quest) == -1)
        return false;

    {
        uint8_t packet[UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH];
        update_quest_completion_time_packet(qid, quest.time, packet);
        session_write(client->session, UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH, packet);
    }

    {
        uint8_t packet[SHOW_EFFECT_PACKET_LENGTH];
        show_effect_packet(0x09, packet);
        session_write(client->session, SHOW_EFFECT_PACKET_LENGTH, packet);
    }

    {
        uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
        show_foreign_effect_packet(chr->id, 0x09, packet);
        session_broadcast_to_room(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
    }

    *success = false;
    const struct QuestInfo *info = wz_get_quest_info(qid);
    uint16_t next_quest = 0;
    for (size_t i = 0; i < info->endActCount; i++) {
        struct QuestAct *act = &info->endActs[i];
        switch (act->type) {
        case QUEST_ACT_TYPE_EXP:
            client_gain_exp(client, act->exp.amount, true);
        break;

        case QUEST_ACT_TYPE_MESO:
            client_gain_meso(client, act->meso.amount, false, true);
        break;

        case QUEST_ACT_TYPE_ITEM: {
            bool has_prop = false;
            int8_t item_count = 0;
            int32_t total = 0;
            for (size_t i = 0; i < act->item.count; i++) {
                if (act->item.items[i].prop == 0 || !has_prop) {
                    if (act->item.items[i].prop != 0)
                        has_prop = true;

                    item_count++;
                }

                total += act->item.items[i].prop;
            }

            uint32_t ids[item_count];
            int16_t amounts[item_count];

            int32_t r;
            if (has_prop) {
                // If there is a random item, there should be at least one empty slot in each inventory
                size_t j;
                for (j = 0; j < chr->equipmentInventory.slotCount; j++)
                    if (chr->equipmentInventory.items[j].isEmpty)
                        break;

                if (j == chr->equipmentInventory.slotCount) {
                    hash_set_u16_remove(chr->completedQuests, qid);
                    return true;
                }

                for (size_t i = 0; i < 4; i++) {
                    size_t j;
                    for (j = 0; j < chr->inventory[i].slotCount; j++) {
                        if (chr->inventory[i].items[j].isEmpty)
                            break;
                    }

                    if (j == chr->inventory[i].slotCount) {
                        hash_set_u16_remove(chr->completedQuests, qid);
                        return true;
                    }
                }
                r = rand() % total;
            }

            has_prop = false;
            item_count = 0;
            total = 0;
            for (size_t j = 0; j < act->item.count; j++) {
                total += act->item.items[j].prop;
                if (act->item.items[j].prop == 0 || (!has_prop && r < total)) {
                    if (act->item.items[j].prop != 0)
                        has_prop = true;

                    ids[item_count] = act->item.items[j].id;
                    amounts[item_count] = act->item.items[j].count;
                    item_count++;
                }
            }

            bool succ;
            if (!client_gain_items(client, item_count, ids, amounts, true, &succ)) {
                hash_set_u16_remove(chr->completedQuests, qid);
                return false;
            }

            if (!succ) {
                hash_set_u16_remove(chr->completedQuests, qid);
                return true;
            }
        }
        break;

        case QUEST_ACT_TYPE_NEXT_QUEST: {
            next_quest = info->endActs[i].nextQuest.qid;
        }
        break;

        case QUEST_ACT_TYPE_SKILL: {
            for (size_t i = 0; i < act->skill.count; i++) {
                for (size_t j = 0; j < act->skill.skills[i].jobCount; j++) {
                    if (chr->job == act->skill.skills[i].jobs[j]) {
                        struct Skill skill = {
                            .id = act->skill.skills[i].id,
                            .level = act->skill.skills[i].level,
                            .masterLevel = act->skill.skills[i].masterLevel,
                        };

                        hash_set_u32_insert(chr->skills, &skill);
                        uint8_t packet[UPDATE_SKILL_PACKET_LENGTH];
                        update_skill_packet(skill.id, skill.level, skill.masterLevel, packet);
                        session_write(client->session, UPDATE_SKILL_PACKET_LENGTH, packet);
                    }
                }

            }
        }
        break;

        default:
            fprintf(stderr, "Unimplemented\n");
        }
    }

    client_commit_stats(client);

    uint8_t packet[END_QUEST_PACKET_LENGTH];
    end_quest_packet(qid, npc, next_quest, packet);
    session_write(client->session, END_QUEST_PACKET_LENGTH, packet);

    hash_set_u16_remove(chr->quests, qid);

    // Sometimes a quest can have an infoNumber of itself, in this case we need to make sure to remove the info number
    // or else in the next login the quest will be reported as active instead of finished
    if (hash_set_u16_get(chr->questInfos, qid) != NULL)
        hash_set_u16_remove(chr->questInfos, qid);

    *success = true;
    return true;
}

static void check_progress(void *data, void *ctx_)
{
    struct CheckProgressContext *ctx = ctx_;
    struct Quest *quest = data;
    const struct QuestInfo *info = wz_get_quest_info(quest->id);

    for (size_t i = 0; i < info->endRequirementCount; i++) {
        if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_MOB) {
            const struct QuestRequirement *req = &info->endRequirements[i];
            for (size_t i = 0; i < req->mob.count; i++) {
                if (req->mob.mobs[i].id == ctx->id && quest->progress[i] < req->mob.mobs[i].count) {
                    quest->progress[i]++;
                    if (quest->progress[i] == req->mob.mobs[i].count) {
                        struct MonsterRefCount *monster = hash_set_u32_get(ctx->monsterQuests, ctx->id);
                        monster->refCount--;
                        if (monster->refCount == 0)
                            hash_set_u32_remove(ctx->monsterQuests, ctx->id);
                    }

                    char progress[15];
                    size_t prog_len = quest_get_progress_string(quest, progress);
                    uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
                    size_t len = update_quest_packet(quest->id, prog_len, progress, packet);
                    session_write(ctx->session, len, packet);
                    break;
                }
            }
            break;
        }
    }
}

static void add_quest(void *data, void *ctx_)
{
    struct Quest *quest = data;
    struct AddQuestContext *ctx = ctx_;

    ctx->quests[ctx->currentQuest] = quest->id;
    ctx->progressCount += quest->progressCount;
    ctx->currentQuest++;
}

static void add_progress(void *data, void *ctx_)
{
    struct Quest *quest = data;
    struct AddProgressContext *ctx = ctx_;
    const struct QuestInfo *info = wz_get_quest_info(quest->id);

    const struct QuestRequirement *req;
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_MOB) {
            req = &info->endRequirements[i];
            break;
        }
    }

    for (uint8_t i = 0; i < quest->progressCount; i++) {
        ctx->progresses[ctx->currentProgress].questId = quest->id;
        ctx->progresses[ctx->currentProgress].progressId = req->mob.mobs[i].id;
        ctx->progresses[ctx->currentProgress].progress = quest->progress[i];
        ctx->currentProgress++;
    }
}

static void add_quest_info(void *data, void *ctx_)
{
    struct QuestInfoProgress *info = data;
    struct AddQuestInfoContext *ctx = ctx_;

    ctx->infos[ctx->currentInfo].infoId = info->id;
    ctx->infos[ctx->currentInfo].progressLength = info->length;
    strncpy(ctx->infos[ctx->currentInfo].progress, info->value, info->length);
    ctx->currentInfo++;
}

static void add_completed_quest(void *data, void *ctx_)
{
    struct CompletedQuest *quest = data;
    struct AddCompletedQuestContext *ctx = ctx_;

    ctx->quests[ctx->currentQuest].id = quest->id;
    struct tm tm;
    gmtime_r(&quest->time, &tm);
    ctx->quests[ctx->currentQuest].time.year = tm.tm_year + 1900;
    ctx->quests[ctx->currentQuest].time.month = tm.tm_mon + 1;
    ctx->quests[ctx->currentQuest].time.day = tm.tm_mday;
    ctx->quests[ctx->currentQuest].time.hour = tm.tm_hour;
    ctx->quests[ctx->currentQuest].time.minute = tm.tm_min;
    ctx->quests[ctx->currentQuest].time.second = tm.tm_sec;
    ctx->quests[ctx->currentQuest].time.second_part = 0;
    ctx->quests[ctx->currentQuest].time.neg = 0;
    ctx->currentQuest++;
}

static void add_skill(void *data, void *ctx_)
{
    struct Skill *skill = data;
    struct AddSkillContext *ctx = ctx_;

    ctx->skills[ctx->currentSkill].id = skill->id;
    ctx->skills[ctx->currentSkill].level = skill->level;
    ctx->skills[ctx->currentSkill].masterLevel = skill->masterLevel;
    ctx->currentSkill++;
}

static void add_monster_book_entry(void *data, void *ctx_)
{
    struct MonsterBookEntry *entry = data;
    struct AddMonsterBookContext *ctx = ctx_;

    ctx->monsterBook[ctx->currentEntry].id = entry->id;
    ctx->monsterBook[ctx->currentEntry].quantity = entry->count;
    ctx->currentEntry++;
}


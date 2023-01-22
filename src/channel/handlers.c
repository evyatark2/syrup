#include "handlers.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../wz.h"
#include "../account.h"
#include "../database.h"
#include "../packet.h"
#include "../hash-map.h"

static uint8_t gender_to_u8(enum CharacterGender gender);

struct LoginHandler {
    struct Client *client;
    struct DatabaseRequest *request;
    uint32_t id;
    int state;
};

struct LoginHandler *login_handler_create(struct Client *client, uint32_t id)
{
    struct LoginHandler *handler = malloc(sizeof(struct LoginHandler));
    if (handler == NULL)
        return NULL;

    handler->client = client;
    handler->id = id;

    handler->state = 0;

    return handler;
}

struct LoginHandlerResult login_handler_handle(struct LoginHandler *handler, int status)
{
    if (handler->state == 0) {
        struct RequestParams params = {
            .type = DATABASE_REQUEST_TYPE_GET_CHARACTER,
            .getCharacter = {
                .id = handler->id,
            }
        };

        handler->request = database_request_create(handler->client->conn, &params);
        handler->state++;
        status = database_request_execute(handler->request, 0);
        if (status < 0) {
            database_request_destroy(handler->request);
            return (struct LoginHandlerResult) { -1 };
        } else if (status > 0) {
            return (struct LoginHandlerResult) { status };
        }
        handler->state++;
    }

    if (handler->state == 1) {
        status = database_request_execute(handler->request, status);
        if (status < 0) {
            database_request_destroy(handler->request);
            return (struct LoginHandlerResult) { -1 };
        } else if (status > 0) {
            return (struct LoginHandlerResult) { status };
        }
        handler->state++;
    }

    if (handler->state == 2) {
        handler->state++;
        struct Character *chr = &handler->client->character;
        const union DatabaseResult *res = database_request_result(handler->request);
        chr->id = handler->id;
        chr->nameLength = res->getCharacter.nameLength;
        memcpy(handler->client->character.name, res->getCharacter.name, res->getCharacter.nameLength);
        // Will be updated in on_client_join()
        //chr->x = info->x;
        //chr->y = info->y;
        //chr->fh = 0;
        //chr->stance = 6;
        chr->map = res->getCharacter.map;
        chr->spawnPoint = wz_get_portal_info_by_name(handler->client->character.map, "sp")->id;
        chr->job = res->getCharacter.job;
        chr->level = res->getCharacter.level;
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
            chr->equippedEquipment[equip_slot_to_compact(equip_slot_from_id(res->getCharacter.equippedEquipment[i].item.itemId))].isEmpty = false;
            struct Equipment *equip = &chr->equippedEquipment[equip_slot_to_compact(equip_slot_from_id(res->getCharacter.equippedEquipment[i].item.itemId))].equip;
            equip->id = res->getCharacter.equippedEquipment[i].id;
            equip->item.id = res->getCharacter.equippedEquipment[i].item.id;
            equip->item.itemId = res->getCharacter.equippedEquipment[i].item.itemId;
            //equip->item.cashId;
            //equip->item.sn; // What is this?

            equip->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
            //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
            equip->item.flags = res->getCharacter.equippedEquipment[i].item.flags;
            equip->item.expiration = -1; //equip->item.expiration = res->getCharacter.equippedEquipment[i].expiration;
            equip->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
            //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
            equip->level = res->getCharacter.equippedEquipment[i].level;
            equip->slots = res->getCharacter.equippedEquipment[i].slots;
            equip->str = res->getCharacter.equippedEquipment[i].str;
            chr->estr += equip->str;
            equip->dex = res->getCharacter.equippedEquipment[i].dex;
            chr->edex += equip->dex;
            equip->int_ = res->getCharacter.equippedEquipment[i].int_;
            chr->eint += equip->int_;
            equip->luk = res->getCharacter.equippedEquipment[i].luk;
            chr->eluk += equip->luk;
            equip->hp = res->getCharacter.equippedEquipment[i].hp;
            chr->eMaxHp += equip->hp;
            equip->mp = res->getCharacter.equippedEquipment[i].mp;
            chr->eMaxMp += equip->mp;
            equip->atk = res->getCharacter.equippedEquipment[i].atk;
            equip->matk = res->getCharacter.equippedEquipment[i].matk;
            equip->def = res->getCharacter.equippedEquipment[i].def;
            equip->mdef = res->getCharacter.equippedEquipment[i].mdef;
            equip->acc = res->getCharacter.equippedEquipment[i].acc;
            equip->avoid = res->getCharacter.equippedEquipment[i].avoid;
            equip->hands = 0; //equip->hands = res->getCharacter.equippedEquipment[i].hands;
            equip->speed = res->getCharacter.equippedEquipment[i].speed;
            equip->jump = res->getCharacter.equippedEquipment[i].jump;
        }

        for (uint8_t j = 0; j < chr->equipmentInventory.slotCount; j++)
            chr->equipmentInventory.items[j].isEmpty = true;

        for (uint8_t i = 0; i < res->getCharacter.equipCount; i++) {
            chr->equipmentInventory.items[res->getCharacter.equipmentInventory[i].slot].isEmpty = false;
            struct Equipment *equip = &chr->equipmentInventory.items[res->getCharacter.equipmentInventory[i].slot].equip;
            equip->id = res->getCharacter.equipmentInventory[i].equip.id;
            equip->item.id = res->getCharacter.equipmentInventory[i].equip.item.id;
            equip->item.itemId = res->getCharacter.equipmentInventory[i].equip.item.itemId;
            //equip->item.cashId;
            //equip->item.sn; // What is this?

            equip->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
            //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
            equip->item.flags = res->getCharacter.equipmentInventory[i].equip.item.flags;
            equip->item.expiration = -1; //equip->item.expiration = res->getCharacter.equippedEquipment[i].expiration;
            equip->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
            //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
            equip->level = res->getCharacter.equipmentInventory[i].equip.level;
            equip->slots = res->getCharacter.equipmentInventory[i].equip.slots;
            equip->str = res->getCharacter.equipmentInventory[i].equip.str;
            equip->dex = res->getCharacter.equipmentInventory[i].equip.dex;
            equip->int_ = res->getCharacter.equipmentInventory[i].equip.int_;
            equip->luk = res->getCharacter.equipmentInventory[i].equip.luk;
            equip->hp = res->getCharacter.equipmentInventory[i].equip.hp;
            equip->mp = res->getCharacter.equipmentInventory[i].equip.mp;
            equip->atk = res->getCharacter.equipmentInventory[i].equip.atk;
            equip->matk = res->getCharacter.equipmentInventory[i].equip.matk;
            equip->def = res->getCharacter.equipmentInventory[i].equip.def;
            equip->mdef = res->getCharacter.equipmentInventory[i].equip.mdef;
            equip->acc = res->getCharacter.equipmentInventory[i].equip.acc;
            equip->avoid = res->getCharacter.equipmentInventory[i].equip.avoid;
            equip->hands = 0; //equip->hands = res->getCharacter.equippedEquipment[i].hands;
            equip->speed = res->getCharacter.equipmentInventory[i].equip.speed;
            equip->jump = res->getCharacter.equipmentInventory[i].equip.jump;
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

        chr->quests = hash_set_u16_create(sizeof(struct Quest), offsetof(struct Quest, id));
        if (chr->quests == NULL) {
            database_request_destroy(handler->request);
            return (struct LoginHandlerResult) { -1 };
        }

        chr->monsterQuests = hash_set_u32_create(sizeof(struct MonsterRefCount), offsetof(struct MonsterRefCount, id));
        if (chr->monsterQuests == NULL) {
            hash_set_u16_destroy(chr->quests);
            chr->quests = NULL;
            database_request_destroy(handler->request);
            return (struct LoginHandlerResult) { -1 };
        }

        for (size_t i = 0; i < res->getCharacter.questCount; i++) {
            struct Quest quest = {
                .id = res->getCharacter.quests[i],
                .progressCount = 0,
            };
            hash_set_u16_insert(chr->quests, &quest); // TODO: Check
                                                      //
            const struct QuestInfo *info = wz_get_quest_info(quest.id);
            for (size_t i = 0; i < info->endRequirementCount; i++) {
                struct QuestRequirement *req = &info->endRequirements[i];
                if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
                }
            }
        }


        for (size_t i = 0; i < res->getCharacter.progressCount; i++) {
            struct Quest *quest = hash_set_u16_get(chr->quests, res->getCharacter.progresses[i].questId); // TODO: Check
            const struct QuestInfo *info = wz_get_quest_info(quest->id);
            uint8_t j;
            int16_t amount;
            for (size_t i_ = 0; i_ < info->endRequirementCount; i_++) {
                struct QuestRequirement *req = &info->endRequirements[i_];
                if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
                    for (size_t i_ = 0; i_ < req->mob.count; i_++) {
                        if (req->mob.mobs[i_].id == res->getCharacter.progresses[i].progressId) {
                            j = i_;
                            amount = req->mob.mobs[i_].count;
                        }
                    }
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

            quest->progress[j].id = res->getCharacter.progresses[i].progressId;
            quest->progress[j].amount = res->getCharacter.progresses[i].progress;
            quest->progressCount++;
        }

        chr->completedQuests = hash_set_u16_create(sizeof(struct CompletedQuest), offsetof(struct CompletedQuest, id));
        if (chr->completedQuests == NULL) {
            hash_set_u32_destroy(chr->monsterQuests);
            chr->monsterQuests = NULL;
            hash_set_u16_destroy(chr->quests);
            chr->quests = NULL;
            database_request_destroy(handler->request);
            return (struct LoginHandlerResult) { -1 };
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
                .time = mktime(&tm)
            };
            hash_set_u16_insert(chr->completedQuests, &quest); // TODO: Check
        }

        chr->skills = hash_set_u32_create(sizeof(struct Skill), offsetof(struct Skill, id));
        if (chr->skills == NULL) {
            hash_set_u16_destroy(chr->completedQuests);
            chr->completedQuests = NULL;
            hash_set_u32_destroy(chr->monsterQuests);
            chr->monsterQuests = NULL;
            hash_set_u16_destroy(chr->quests);
            chr->quests = NULL;
            database_request_destroy(handler->request);
            return (struct LoginHandlerResult) { -1 };
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
            hash_set_u32_destroy(chr->monsterQuests);
            chr->monsterQuests = NULL;
            hash_set_u16_destroy(chr->quests);
            chr->quests = NULL;
            database_request_destroy(handler->request);
            return (struct LoginHandlerResult) { -1 };
        }

        for (size_t i = 0; i < res->getCharacter.monsterBookEntryCount; i++) {
            struct MonsterBookEntry entry = {
                .id = res->getCharacter.monsterBook[i].id,
                .count = res->getCharacter.monsterBook[i].quantity,
            };
            hash_set_u32_insert(chr->monsterBook, &entry);
        }

        database_request_destroy(handler->request);

        {
            uint8_t packet[ENTER_MAP_PACKET_MAX_LENGTH];
            size_t len = enter_map_packet(&handler->client->character, packet);
            session_write(handler->client->session, len, packet);
        }

        session_write(handler->client->session, 2, (uint8_t[]) { 0x23, 0x00 }); // Force stat reset

        {
            uint8_t packet[SET_GENDER_PACKET_LENGTH];
            set_gender_packet(handler->client->character.gender, packet);
            session_write(handler->client->session, SET_GENDER_PACKET_LENGTH, packet);
        }

        session_write(handler->client->session, 3, (uint8_t[]) { 0x2F, 0x00, 0x01 });

        struct LoginHandlerResult ret = { 0 };
        //ret.size = enter_map_packet(&handler->client->character, ret.packet);
        return ret;
    }

    return (struct LoginHandlerResult) { -1 };
}

void login_handler_destroy(struct LoginHandler *handler)
{
    free(handler);
}

struct ChangeMapResult handle_change_map(struct Client *client, uint32_t map, uint8_t target)
{
    client->character.map = map;
    struct ChangeMapResult ret = { 0 };
    change_map_packet(&client->character, map, target, ret.packet);
    ret.size = CHANGE_MAP_PACKET_LENGTH;
    return ret;
}

struct LogoutHandler {
    struct Client *client;
    struct DatabaseRequest *request;
    int state;
    void *quests;
    void *progresses;
    void *completedQuests;
    void *skills;
    void *monsterBook;
};

struct LogoutHandler *logout_handler_create(struct Client *client)
{
    struct LogoutHandler *handler = malloc(sizeof(struct LogoutHandler));
    if (handler == NULL)
        return NULL;

    handler->client = client;
    handler->state = 0;

    return handler;
}

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

int logout_handler_handle(struct LogoutHandler *handler, int status)
{
    struct Character *chr = &handler->client->character;
    if (handler->state == 0) {
        struct RequestParams params = {
            .type = DATABASE_REQUEST_TYPE_UPDATE_CHARACTER,
            .updateCharacter = {
                .id = chr->id,
                .map = chr->map,
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
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].id = chr->equippedEquipment[i].equip.id;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].item.id = chr->equippedEquipment[i].equip.item.id;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].item.itemId = chr->equippedEquipment[i].equip.item.itemId;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].item.flags = chr->equippedEquipment[i].equip.item.flags;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].item.ownerLength = chr->equippedEquipment[i].equip.item.ownerLength;
                memcpy(params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].item.owner, chr->equippedEquipment[i].equip.item.owner, chr->equippedEquipment[i].equip.item.ownerLength);
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].item.giverLength = chr->equippedEquipment[i].equip.item.giftFromLength;
                memcpy(params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].item.giver, chr->equippedEquipment[i].equip.item.giftFrom, chr->equippedEquipment[i].equip.item.giftFromLength);


                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].level = chr->equippedEquipment[i].equip.level;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].slots = chr->equippedEquipment[i].equip.slots;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].str = chr->equippedEquipment[i].equip.str;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].dex = chr->equippedEquipment[i].equip.dex;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].int_ = chr->equippedEquipment[i].equip.int_;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].luk = chr->equippedEquipment[i].equip.luk;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].hp = chr->equippedEquipment[i].equip.hp;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].mp = chr->equippedEquipment[i].equip.mp;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].atk = chr->equippedEquipment[i].equip.atk;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].matk = chr->equippedEquipment[i].equip.matk;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].def = chr->equippedEquipment[i].equip.def;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].mdef = chr->equippedEquipment[i].equip.mdef;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].acc = chr->equippedEquipment[i].equip.acc;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].avoid = chr->equippedEquipment[i].equip.avoid;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].speed = chr->equippedEquipment[i].equip.speed;
                params.updateCharacter.equippedEquipment[params.updateCharacter.equippedCount].jump = chr->equippedEquipment[i].equip.jump;

                params.updateCharacter.equippedCount++;
            }
        }

        for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
            if (!chr->equipmentInventory.items[i].isEmpty) {
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.id = chr->equipmentInventory.items[i].equip.id;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].slot = i;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.item.id = chr->equipmentInventory.items[i].equip.item.id;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.item.itemId = chr->equipmentInventory.items[i].equip.item.itemId;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.item.flags = chr->equipmentInventory.items[i].equip.item.flags;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.item.ownerLength = chr->equipmentInventory.items[i].equip.item.ownerLength;
                memcpy(params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.item.owner, chr->equipmentInventory.items[i].equip.item.owner, chr->equipmentInventory.items[i].equip.item.ownerLength);
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.item.giverLength = chr->equipmentInventory.items[i].equip.item.giftFromLength;
                memcpy(params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.item.giver, chr->equipmentInventory.items[i].equip.item.giftFrom, chr->equipmentInventory.items[i].equip.item.giftFromLength);


                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.level = chr->equipmentInventory.items[i].equip.level;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.slots = chr->equipmentInventory.items[i].equip.slots;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.str = chr->equipmentInventory.items[i].equip.str;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.dex = chr->equipmentInventory.items[i].equip.dex;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.int_ = chr->equipmentInventory.items[i].equip.int_;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.luk = chr->equipmentInventory.items[i].equip.luk;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.hp = chr->equipmentInventory.items[i].equip.hp;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.mp = chr->equipmentInventory.items[i].equip.mp;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.atk = chr->equipmentInventory.items[i].equip.atk;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.matk = chr->equipmentInventory.items[i].equip.matk;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.def = chr->equipmentInventory.items[i].equip.def;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.mdef = chr->equipmentInventory.items[i].equip.mdef;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.acc = chr->equipmentInventory.items[i].equip.acc;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.avoid = chr->equipmentInventory.items[i].equip.avoid;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.speed = chr->equipmentInventory.items[i].equip.speed;
                params.updateCharacter.equipmentInventory[params.updateCharacter.equipCount].equip.jump = chr->equipmentInventory.items[i].equip.jump;

                params.updateCharacter.equipCount++;
            }
        }

        for (uint8_t inv = 0; inv < 4; inv++) {
            for (uint8_t i = 0; i < chr->inventory[inv].slotCount; i++) {
                if (!chr->inventory[inv].items[i].isEmpty) {
                    params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item.id = chr->inventory[inv].items[i].item.item.id;
                    params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item.itemId = chr->inventory[inv].items[i].item.item.itemId;
                    params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item.flags = chr->inventory[inv].items[i].item.item.flags;
                    params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item.ownerLength = chr->inventory[inv].items[i].item.item.ownerLength;
                    memcpy(params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item.owner, chr->inventory[inv].items[i].item.item.owner, chr->inventory[inv].items[i].item.item.ownerLength);
                    params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item.giverLength = chr->inventory[inv].items[i].item.item.giftFromLength;
                    memcpy(params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].item.giver, chr->inventory[inv].items[i].item.item.giftFrom, chr->inventory[inv].items[i].item.item.giftFromLength);
                    params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].slot = i;
                    params.updateCharacter.inventoryItems[params.updateCharacter.itemCount].count = chr->inventory[inv].items[i].item.quantity;

                    params.updateCharacter.itemCount++;
                }
            }
        }

        params.updateCharacter.quests = malloc(hash_set_u16_size(chr->quests) * sizeof(uint16_t));
        if (params.updateCharacter.quests == NULL)
            return -1;

        handler->quests = params.updateCharacter.quests;

        struct AddQuestContext ctx = {
            .quests = params.updateCharacter.quests,
            .currentQuest = 0,
            .progressCount = 0
        };
        hash_set_u16_foreach(chr->quests, add_quest, &ctx);

        params.updateCharacter.questCount = ctx.currentQuest;

        params.updateCharacter.progresses = malloc(ctx.progressCount * sizeof(struct DatabaseProgress));
        if (params.updateCharacter.progresses == NULL) {
            free(handler->quests);
            return -1;
        }

        handler->progresses = params.updateCharacter.progresses;

        struct AddProgressContext ctx2 = {
            .progresses = params.updateCharacter.progresses,
            .currentProgress = 0
        };
        hash_set_u16_foreach(chr->quests, add_progress, &ctx2);

        params.updateCharacter.progressCount = ctx2.currentProgress;

        params.updateCharacter.completedQuests = malloc(hash_set_u16_size(chr->completedQuests) * sizeof(struct DatabaseCompletedQuest));
        if (params.updateCharacter.completedQuests == NULL) {
            free(handler->progresses);
            free(handler->quests);
            return -1;
        }

        handler->completedQuests = params.updateCharacter.completedQuests;

        struct AddCompletedQuestContext ctx3 = {
            .quests = params.updateCharacter.completedQuests,
            .currentQuest = 0,
        };
        hash_set_u16_foreach(chr->completedQuests, add_completed_quest, &ctx3);

        params.updateCharacter.completedQuestCount = ctx3.currentQuest;

        params.updateCharacter.skills = malloc(hash_set_u32_size(chr->skills) * sizeof(struct DatabaseSkill));
        if (params.updateCharacter.completedQuests == NULL) {
            free(handler->completedQuests);
            free(handler->progresses);
            free(handler->quests);
            return -1;
        }

        handler->skills = params.updateCharacter.skills;

        struct AddSkillContext ctx4 = {
            .skills = params.updateCharacter.skills,
            .currentSkill = 0,
        };
        hash_set_u32_foreach(chr->skills, add_skill, &ctx4);

        params.updateCharacter.skillCount = ctx4.currentSkill;

        params.updateCharacter.monsterBook = malloc(hash_set_u32_size(chr->monsterBook) * sizeof(struct DatabaseMonsterBookEntry));
        if (params.updateCharacter.monsterBook == NULL) {
            free(handler->skills);
            free(handler->completedQuests);
            free(handler->progresses);
            free(handler->quests);
            return -1;
        }

        handler->monsterBook = params.updateCharacter.monsterBook;

        struct AddMonsterBookContext ctx5 = {
            .monsterBook = params.updateCharacter.monsterBook,
            .currentEntry = 0,
        };
        hash_set_u32_foreach(chr->monsterBook, add_monster_book_entry, &ctx5);

        params.updateCharacter.monsterBookEntryCount = ctx5.currentEntry;

        handler->request = database_request_create(handler->client->conn, &params);
        if (handler->request == NULL) {
            free(handler->skills);
            free(handler->completedQuests);
            free(handler->progresses);
            free(handler->quests);
            return -1;
        }

        handler->state++;
        status = database_request_execute(handler->request, 0);
        if (status != 0) {
            if (status < 0) {
                free(handler->skills);
                free(handler->completedQuests);
                free(handler->progresses);
                free(handler->quests);
                database_request_destroy(handler->request);
            }

            return status;
        }
        handler->state++;
    }

    if (handler->state == 1) {
        status = database_request_execute(handler->request, status);
        if (status <= 0) {
            free(handler->skills);
            free(handler->completedQuests);
            free(handler->progresses);
            free(handler->quests);
            database_request_destroy(handler->request);
        }
        return status;
    }

    return 0;
}

void logout_handler_destroy(struct LogoutHandler *handler)
{
    free(handler);
}

struct MovePlayerResult handle_move_player(struct Client *client, size_t len, uint8_t *data)
{
    struct MovePlayerResult res;
    res.size = move_player_packet(client->character.id, len, data, res.packet);
    return res;
}

static uint8_t gender_to_u8(enum CharacterGender gender)
{
    return gender == CHARACTER_GENDER_MALE ? 0 : 1;
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

    for (uint8_t i = 0; i < quest->progressCount; i++) {
        ctx->progresses[ctx->currentProgress].questId = quest->id;
        ctx->progresses[ctx->currentProgress].progressId = quest->progress[i].id;
        ctx->progresses[ctx->currentProgress].progress = quest->progress[i].amount;
        ctx->currentProgress++;
    }
}

static void add_completed_quest(void *data, void *ctx_)
{
    struct CompletedQuest *quest = data;
    struct AddCompletedQuestContext *ctx = ctx_;

    ctx->quests[ctx->currentQuest].id = quest->id;
    struct tm tm;
    gmtime_r(&quest->time, &tm);
    ctx->quests[ctx->currentQuest].time.year = tm.tm_year;
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


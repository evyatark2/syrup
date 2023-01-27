#include "drops.h"

#include <cmph.h>

struct DropInfoNode {
    uint32_t id;
    struct MonsterDropInfo info;
};

size_t DROP_INFO_COUNT;
struct DropInfoNode *DROP_INFOS;
cmph_t *DROP_INFO_MPH;

size_t REACTOR_DROP_INFO_COUNT;
struct DropInfoNode *REACTOR_DROP_INFOS;
cmph_t *REACTOR_DROP_INFO_MPH;


int drops_load_from_db(struct DatabaseConnection *conn)
{
    struct RequestParams params = {
        .type = DATABASE_REQUEST_TYPE_GET_MONSTER_DROPS
    };
    struct DatabaseRequest *req = database_request_create(conn, &params);

    if (database_request_execute(req, 0) == -1) {
        database_request_destroy(req);
        return -1;
    }

    const union DatabaseResult *res = database_request_result(req);
    DROP_INFOS = malloc(res->getMonsterDrops.count * sizeof(struct DropInfoNode));
    for (size_t i = 0; i < res->getMonsterDrops.count; i++) {
        const struct MonsterDrops *monster = &res->getMonsterDrops.monsters[i];
        DROP_INFOS[DROP_INFO_COUNT].id = monster->id;
        DROP_INFOS[DROP_INFO_COUNT].info.drops = malloc((monster->itemDrops.count + monster->questItemDrops.count + (monster->mesoDrop.max != 0 ? 1 : 0) + monster->multiItemDrops.count) * sizeof(struct DropInfo));
        DROP_INFOS[DROP_INFO_COUNT].info.count = 0;
        for (size_t i = 0; i < monster->itemDrops.count; i++) {
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].itemId = monster->itemDrops.drops[i].itemId;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].isQuest = false;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].min = 1;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].max = 1;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].chance = monster->itemDrops.drops[i].chance;
            DROP_INFOS[DROP_INFO_COUNT].info.count++;
        }

        for (size_t i = 0; i < monster->questItemDrops.count; i++) {
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].itemId = monster->questItemDrops.drops[i].itemId;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].isQuest = true;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].questId = monster->questItemDrops.drops[i].questId;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].min = 1;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].max = 1;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].chance = monster->questItemDrops.drops[i].chance;
            DROP_INFOS[DROP_INFO_COUNT].info.count++;
        }

        if (monster->mesoDrop.max != 0) {
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].itemId = 0;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].isQuest = false;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].min = monster->mesoDrop.min;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].max = monster->mesoDrop.max;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].chance = monster->mesoDrop.chance;
            DROP_INFOS[DROP_INFO_COUNT].info.count++;
        }

        for (size_t i = 0; i < monster->multiItemDrops.count; i++) {
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].itemId = monster->multiItemDrops.drops[i].id;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].isQuest = false;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].min = monster->multiItemDrops.drops[i].min;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].max = monster->multiItemDrops.drops[i].max;
            DROP_INFOS[DROP_INFO_COUNT].info.drops[DROP_INFOS[DROP_INFO_COUNT].info.count].chance = monster->multiItemDrops.drops[i].chance;
            DROP_INFOS[DROP_INFO_COUNT].info.count++;
        }

        DROP_INFO_COUNT++;
    }

    database_request_destroy(req);

    cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(DROP_INFOS, sizeof(struct DropInfoNode), offsetof(struct DropInfoNode, id), sizeof(uint32_t), DROP_INFO_COUNT);
    cmph_config_t *config = cmph_config_new(adapter);
    cmph_config_set_algo(config, CMPH_BDZ);
    DROP_INFO_MPH = cmph_new(config);
    cmph_config_destroy(config);
    cmph_io_struct_vector_adapter_destroy(adapter);

    size_t i = 0;
    while (i < DROP_INFO_COUNT) {
        cmph_uint32 j = cmph_search(DROP_INFO_MPH, (void *)&DROP_INFOS[i].id, sizeof(uint32_t));
        if (i != j) {
            struct DropInfoNode temp = DROP_INFOS[j];
            DROP_INFOS[j] = DROP_INFOS[i];
            DROP_INFOS[i] = temp;
        } else {
            i++;
        }
    }

    params.type = DATABASE_REQUEST_TYPE_GET_REACTOR_DROPS;
    req = database_request_create(conn, &params);

    if (database_request_execute(req, 0) == -1) {
        database_request_destroy(req);
        return -1;
    }

    res = database_request_result(req);
    REACTOR_DROP_INFOS = malloc(res->getReactorDrops.count * sizeof(struct DropInfoNode));
    for (size_t i = 0; i < res->getReactorDrops.count; i++) {
        const struct ReactorDrops *reactor = &res->getReactorDrops.reactors[i];
        REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].id = reactor->id;
        REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops = malloc((reactor->itemDrops.count + reactor->questItemDrops.count) * sizeof(struct DropInfo));
        REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count = 0;
        for (size_t i = 0; i < reactor->itemDrops.count; i++) {
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].itemId = reactor->itemDrops.drops[i].itemId;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].isQuest = false;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].min = 1;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].max = 1;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].chance = 1000000 / reactor->itemDrops.drops[i].chance;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count++;
        }

        for (size_t i = 0; i < reactor->questItemDrops.count; i++) {
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].itemId = reactor->questItemDrops.drops[i].itemId;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].isQuest = true;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].questId = reactor->questItemDrops.drops[i].questId;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].min = 1;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].max = 1;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.drops[REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count].chance = 1000000 / reactor->questItemDrops.drops[i].chance;
            REACTOR_DROP_INFOS[REACTOR_DROP_INFO_COUNT].info.count++;
        }

        REACTOR_DROP_INFO_COUNT++;
    }

    database_request_destroy(req);

    adapter = cmph_io_struct_vector_adapter(REACTOR_DROP_INFOS, sizeof(struct DropInfoNode), offsetof(struct DropInfoNode, id), sizeof(uint32_t), REACTOR_DROP_INFO_COUNT);
    config = cmph_config_new(adapter);
    cmph_config_set_algo(config, CMPH_BDZ);
    REACTOR_DROP_INFO_MPH = cmph_new(config);
    cmph_config_destroy(config);
    cmph_io_struct_vector_adapter_destroy(adapter);

    i = 0;
    while (i < REACTOR_DROP_INFO_COUNT) {
        cmph_uint32 j = cmph_search(REACTOR_DROP_INFO_MPH, (void *)&REACTOR_DROP_INFOS[i].id, sizeof(uint32_t));
        if (i != j) {
            struct DropInfoNode temp = REACTOR_DROP_INFOS[j];
            REACTOR_DROP_INFOS[j] = REACTOR_DROP_INFOS[i];
            REACTOR_DROP_INFOS[i] = temp;
        } else {
            i++;
        }
    }

    return 0;
}

void drops_unload(void)
{
    cmph_destroy(DROP_INFO_MPH);
    for (size_t i = 0; i < DROP_INFO_COUNT; i++) {
        free(DROP_INFOS[i].info.drops);
    }
    free(DROP_INFOS);
}

const struct MonsterDropInfo *drop_info_find(uint32_t id)
{
    cmph_uint32 index = cmph_search(DROP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    if (DROP_INFOS[index].id != id)
        return NULL;

    return &DROP_INFOS[index].info;
}

const struct MonsterDropInfo *reactor_drop_info_find(uint32_t id)
{
    cmph_uint32 index = cmph_search(REACTOR_DROP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    if (REACTOR_DROP_INFOS[index].id != id)
        return NULL;

    return &REACTOR_DROP_INFOS[index].info;
}


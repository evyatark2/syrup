#ifndef DROPS_H
#define DROPS_H

#include <stdbool.h>
#include <stdint.h>

#include "../database.h"

struct DropInfo {
    uint32_t itemId;
    int32_t min;
    int32_t max;
    bool isQuest;
    uint16_t questId;
    int32_t chance;
};

struct MonsterDropInfo {
    size_t count;
    struct DropInfo *drops;
};

int drops_load_from_db(struct DatabaseConnection *conn);
void drops_unload(void);
const struct MonsterDropInfo *drop_info_find(uint32_t id);
const struct MonsterDropInfo *reactor_drop_info_find(uint32_t id);

#endif


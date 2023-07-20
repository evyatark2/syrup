#ifndef STAT_H
#define STAT_H

#include <stdint.h>

#include "../character.h"

enum Stat : uint32_t {
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

#endif


#ifndef DROP_H
#define DROP_H

#include <stdint.h>

#include "../item.h"

enum DropType {
    DROP_TYPE_MESO,
    DROP_TYPE_ITEM,
    DROP_TYPE_EQUIP
};

struct Drop {
    uint32_t oid;
    enum DropType type;
    int16_t x;
    int16_t y;
    union {
        int32_t meso;
        struct {
            uint16_t qid;
            struct InventoryItem item;
        };
        struct Equipment equip;
    };
};

#endif


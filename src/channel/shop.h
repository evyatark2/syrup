#ifndef SHOP_H
#define SHOP_H

#include <stdbool.h>
#include <stdint.h>

#include "../database.h"

struct ShopItemInfo {
    uint32_t id;
    int32_t price;
};

struct ShopInfo {
    size_t count;
    struct ShopItemInfo *items;
};

int shops_load_from_db(struct DatabaseConnection *conn);
void shops_unload();
const struct ShopInfo *shop_info_find(uint32_t id);

#endif


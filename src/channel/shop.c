#include "shop.h"

#include <cmph.h>

struct ShopInfoNode {
    uint32_t id;
    struct ShopInfo info;
};

size_t SHOP_INFO_COUNT;
struct ShopInfoNode *SHOP_INFOS;
cmph_t *SHOP_INFO_MPH;

int shops_load_from_db(struct DatabaseConnection *conn)
{
    struct RequestParams params = {
        .type = DATABASE_REQUEST_TYPE_GET_SHOPS
    };
    struct DatabaseRequest *req = database_request_create(conn, &params);

    if (database_request_execute(req, 0) == -1) {
        database_request_destroy(req);
        return -1;
    }

    const union DatabaseResult *res = database_request_result(req);
    SHOP_INFOS = malloc(res->getShops.count * sizeof(struct ShopInfoNode));
    for (size_t i = 0; i < res->getShops.count; i++) {
        const struct Shop *shop = &res->getShops.shops[i];
        SHOP_INFOS[SHOP_INFO_COUNT].id = shop->id;
        SHOP_INFOS[SHOP_INFO_COUNT].info.items = malloc(shop->count * sizeof(struct ShopItemInfo));
        SHOP_INFOS[SHOP_INFO_COUNT].info.count = 0;
        for (size_t i = 0; i < shop->count; i++) {
            SHOP_INFOS[SHOP_INFO_COUNT].info.items[SHOP_INFOS[SHOP_INFO_COUNT].info.count].id = shop->items[i].id;
            SHOP_INFOS[SHOP_INFO_COUNT].info.items[SHOP_INFOS[SHOP_INFO_COUNT].info.count].price = shop->items[i].price;
            SHOP_INFOS[SHOP_INFO_COUNT].info.count++;
        }

        SHOP_INFO_COUNT++;
    }

    database_request_destroy(req);

    cmph_io_adapter_t *adapter = cmph_io_struct_vector_adapter(SHOP_INFOS, sizeof(struct ShopInfoNode), offsetof(struct ShopInfoNode, id), sizeof(uint32_t), SHOP_INFO_COUNT);
    cmph_config_t *config = cmph_config_new(adapter);
    cmph_config_set_algo(config, CMPH_BDZ);
    SHOP_INFO_MPH = cmph_new(config);
    cmph_config_destroy(config);
    cmph_io_struct_vector_adapter_destroy(adapter);

    size_t i = 0;
    while (i < SHOP_INFO_COUNT) {
        cmph_uint32 j = cmph_search(SHOP_INFO_MPH, (void *)&SHOP_INFOS[i].id, sizeof(uint32_t));
        if (i != j) {
            struct ShopInfoNode temp = SHOP_INFOS[j];
            SHOP_INFOS[j] = SHOP_INFOS[i];
            SHOP_INFOS[i] = temp;
        } else {
            i++;
        }
    }

    return 0;
}

void shops_unload(void)
{
    cmph_destroy(SHOP_INFO_MPH);
    for (size_t i = 0; i < SHOP_INFO_COUNT; i++) {
        free(SHOP_INFOS[i].info.items);
    }
    free(SHOP_INFOS);
}

const struct ShopInfo *shop_info_find(uint32_t id)
{
    cmph_uint32 index = cmph_search(SHOP_INFO_MPH, (void *)&id, sizeof(uint32_t));
    if (SHOP_INFOS[index].id != id)
        return NULL;

    return &SHOP_INFOS[index].info;
}


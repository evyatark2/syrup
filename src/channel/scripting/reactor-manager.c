#include "reactor-manager.h"

#include <stdlib.h>

#include <lauxlib.h>

#include "../map.h"
#include "../client.h"
#include "../drops.h"
#include "../../wz.h"

struct ReactorManager {
    struct Map *map;
    struct Client *client;
    size_t reactorIndex; // Index of the reactor in the map info
    uint32_t oid;
};

struct ReactorManager *reactor_manager_create(struct Map *map, struct Client *client, size_t i, uint32_t oid)
{
    struct ReactorManager *rm = malloc(sizeof(struct ReactorManager));
    if (rm == NULL)
        return NULL;

    rm->map = map;
    rm->client = client;
    rm->reactorIndex = i;
    rm->oid = oid;

    return rm;
}

void reactor_manager_destroy(struct ReactorManager *rm)
{
    free(rm);
}

static int l_reactor_manager_drop(lua_State *L);

static const struct luaL_Reg reactor_manager_lib[] = {
    { "drop", l_reactor_manager_drop },
    { NULL, NULL }
};

int luaopen_reactor_manager(lua_State *L)
{
    luaL_newmetatable(L, SCRIPT_REACTOR_MANAGER_TYPE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, reactor_manager_lib, 0);
    return 1;
}

static int l_reactor_manager_drop(lua_State *L)
{
    struct ReactorManager *rm = *(void **)luaL_checkudata(L, 1, SCRIPT_REACTOR_MANAGER_TYPE);

    const struct MapReactorInfo *reactor = &wz_get_reactors_for_map(map_get_id(rm->map), NULL)[rm->reactorIndex];
    const struct MonsterDropInfo *info = reactor_drop_info_find(reactor->id);
    struct Drop drops[info->count];

    size_t drop_count = 0;
    for (size_t i = 0; i < info->count; i++) {
        if (rand() % 1000000 < info->drops[i].chance * 16) {
            drops[drop_count].type = info->drops[i].itemId == 0 ? DROP_TYPE_MESO : (info->drops[i].itemId / 1000000 == 1 ? DROP_TYPE_EQUIP : DROP_TYPE_ITEM);
            switch (drops[drop_count].type) {
            case DROP_TYPE_MESO:
                drops[drop_count].meso = rand() % (info->drops[i].max - info->drops[i].min + 1) + info->drops[i].min;
            break;

            case DROP_TYPE_ITEM:
                drops[drop_count].qid = info->drops[i].isQuest ? info->drops[i].questId : 0;
                drops[drop_count].item.item.id = 0;
                drops[drop_count].item.item.itemId = info->drops[i].itemId;
                drops[drop_count].item.item.ownerLength = 0;
                drops[drop_count].item.item.flags = 0;
                drops[drop_count].item.item.expiration = -1;
                drops[drop_count].item.item.giftFromLength = 0;
                drops[drop_count].item.quantity = rand() % (info->drops[i].max - info->drops[i].min + 1) + info->drops[i].min;
            break;

            case DROP_TYPE_EQUIP:
                drops[drop_count].equip = equipment_from_info(wz_get_equip_info(info->drops[i].itemId));
            break;
            }
            drop_count++;
        }
    }

    if (map_drop_batch_from_map_object(rm->map, client_get_character(rm->client)->id, rm->oid, drop_count, drops) == -1)
        return luaL_error(L, "");
    return 0;
}


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

    if (map_drop_batch_from_reactor(rm->map, client_get_map(rm->client)->player, rm->oid) == -1)
        return luaL_error(L, "");
    return 0;
}


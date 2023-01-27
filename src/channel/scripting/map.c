#include "map.h"

#include <lauxlib.h>

#include "../map.h"
#include "client.h"
#include "script-manager.h"

static int l_map_drop_batch_from_oid(lua_State *L);

static const struct luaL_Reg maplib[] = {
    { "drop", l_map_drop_batch_from_oid },
    { NULL, NULL }
};

int luaopen_map(lua_State *L)
{
    luaL_newmetatable(L, SCRIPT_MAP_TYPE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, maplib, 0);
    return 1;
}

static int l_map_drop_batch_from_oid(lua_State *L)
{
    struct Map *map = *(void **)luaL_checkudata(L, 1, SCRIPT_MAP_TYPE);
    struct Client *client = *(void **)luaL_checkudata(L, 2, SCRIPT_CLIENT_TYPE);
    uint32_t oid = luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    struct Drop drops[luaL_len(L, 4)];
    for (int i = 1; i <= luaL_len(L, 4); i++) {
    }

    lua_pushinteger(L, map_drop_batch_from_map_object(map, client_get_character(client)->id, oid, luaL_len(L, 4), drops));
    return 1;
}


#include "event.h"

#include <lauxlib.h>

static int l_get_event(lua_State *L);

static int l_event_get_property(lua_State *L);

static const struct luaL_Reg eventlib[] = {
    { "getProperty", l_event_get_property },
    { NULL, NULL }
};


void luaopen_event(lua_State *L, struct ChannelServer *server)
{
    luaL_newmetatable(L, SCRIPT_EVENT_TYPE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, eventlib, 0);
    lua_pushlightuserdata(L, server);
    lua_pushcclosure(L, l_get_event, 1);
    lua_setglobal(L, "getEvent");
}

static int l_get_event(lua_State *L)
{
    void *server = lua_touserdata(L, lua_upvalueindex(1));
    channel_server_get_event(server, luaL_checkinteger(L, 1));
    
    void **data = lua_newuserdata(L, sizeof(struct Event *));
    *data = channel_server_get_event(server, luaL_checkinteger(L, 1));
    luaL_getmetatable(L, SCRIPT_EVENT_TYPE);
    lua_setmetatable(L, -2);

    return 1;
}

static int l_event_get_property(lua_State *L)
{
    struct Event *event = *(void **)luaL_checkudata(L, 1, SCRIPT_EVENT_TYPE);

    uint32_t property = luaL_checkinteger(L, 2);
    lua_pushinteger(L, event_get_property(event, property));

    return 1;
}


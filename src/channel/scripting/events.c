#include "events.h"

#include "../events.h"

void luaopen_events(lua_State *L)
{
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, EVENT_BOAT);
    lua_setfield(L, -2, "BOAT");
    lua_pushinteger(L, EVENT_TRAIN);
    lua_setfield(L, -2, "TRAIN");
    lua_pushinteger(L, EVENT_SUBWAY);
    lua_setfield(L, -2, "SUBWAY");
    lua_pushinteger(L, EVENT_GENIE);
    lua_setfield(L, -2, "GENIE");
    lua_pushinteger(L, EVENT_AIRPLANE);
    lua_setfield(L, -2, "AIRPLANE");
    lua_pushinteger(L, EVENT_ELEVATOR);
    lua_setfield(L, -2, "ELEVATOR");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "SAILING");
}

#include "lua.h"

#include "../server.h"

#define SCRIPT_EVENT_TYPE "Syrup.event"

void luaopen_event(lua_State *L, struct ChannelServer *server);


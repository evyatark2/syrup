#include <stddef.h>
#include <stdint.h>

#include "lua.h"

#include "../room.h"

#define SCRIPT_REACTOR_MANAGER_TYPE "Syrup.rm"

int luaopen_reactor_manager(lua_State *L);

struct ReactorManager;

struct ReactorManager *reactor_manager_create(struct Room *room, struct RoomMember *member, uint32_t oid, uint32_t id);
void reactor_manager_destroy(struct ReactorManager *rm);


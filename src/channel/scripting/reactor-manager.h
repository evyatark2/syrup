#include <stddef.h>
#include <stdint.h>

#include "lua.h"

#include "../map.h"
#include "../client.h"

#define SCRIPT_REACTOR_MANAGER_TYPE "Syrup.rm"

int luaopen_reactor_manager(lua_State *L);

struct ReactorManager;

struct ReactorManager *reactor_manager_create(struct Map *map, struct Client *client, size_t i, uint32_t oid);
void reactor_manager_destroy(struct ReactorManager *rm);


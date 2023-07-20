#include "reactor-manager.h"

#include <stdlib.h>

#include <lauxlib.h>

#include "user.h"
#include "../room.h"
#include "../drops.h"
#include "../../wz.h"

struct ReactorManager {
    struct Room *room;
    struct RoomMember *member;
    uint32_t oid;
    uint32_t id;
};

struct ReactorManager *reactor_manager_create(struct Room *room, struct RoomMember *member, uint32_t oid, uint32_t id)
{
    struct ReactorManager *rm = malloc(sizeof(struct ReactorManager));
    if (rm == NULL)
        return NULL;

    rm->room = room;
    rm->member = member;
    rm->oid = oid;
    rm->id = id;

    return rm;
}

void reactor_manager_destroy(struct ReactorManager *rm)
{
    free(rm);
}

static int l_reactor_manager_member(lua_State *L);
static int l_reactor_manager_drop(lua_State *L);
static int l_reactor_manager_id(lua_State *L);

static const struct luaL_Reg reactor_manager_lib[] = {
    { "member", l_reactor_manager_member },
    { "drop", l_reactor_manager_drop },
    { "id", l_reactor_manager_id },
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

static int l_reactor_manager_member(lua_State *L)
{
    struct ReactorManager *rm = *(void **)luaL_checkudata(L, 1, SCRIPT_REACTOR_MANAGER_TYPE);

    void **data = lua_newuserdata(L, sizeof(void *));
    *data = rm->member;
    luaL_getmetatable(L, SCRIPT_USER_TYPE);
    lua_setmetatable(L, -2);

    return 1;
}

static int l_reactor_manager_drop(lua_State *L)
{
    /*struct ReactorManager *rm = *(void **)luaL_checkudata(L, 1, SCRIPT_REACTOR_MANAGER_TYPE);

    if (room_member_drop_from_reactor(rm->room, rm->member, rm->oid) == -1)
        return luaL_error(L, "");

        */
    return 0;
}

static int l_reactor_manager_id(lua_State *L)
{
    struct ReactorManager *rm = *(void **)luaL_checkudata(L, 1, SCRIPT_REACTOR_MANAGER_TYPE);
    lua_pushinteger(L, rm->id);
    return 1;
}


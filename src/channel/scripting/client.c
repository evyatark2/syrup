#include "client.h"

#include <lauxlib.h>

#include "../client.h"
#include "script-manager.h"

static int l_client_active_npc(lua_State *L);
static int l_client_enable_actions(lua_State *L);
static int l_client_end_quest_now(lua_State *L);
static int l_client_change_job(lua_State *L);
static int l_client_gain_exp(lua_State *L);
static int l_client_gain_fame(lua_State *L);
static int l_client_gain_hp(lua_State *L);
static int l_client_gain_items(lua_State *L);
static int l_client_gain_meso(lua_State *L);
static int l_client_gain_mp(lua_State *L);
static int l_client_gender(lua_State *L);
static int l_client_hp(lua_State *L);
static int l_client_max_hp(lua_State *L);
static int l_client_max_mp(lua_State *L);
static int l_client_mp(lua_State *L);
static int l_client_open_shop(lua_State *L);
static int l_client_job(lua_State *L);
static int l_client_has_item(lua_State *L);
static int l_client_is_quest_started(lua_State *L);
static int l_client_is_quest_complete(lua_State *L);
static int l_client_level(lua_State *L);
static int l_client_meso(lua_State *L);
static int l_client_send_accept_decline(lua_State *L);
static int l_client_send_simple(lua_State *L);
static int l_client_send_next(lua_State *L);
static int l_client_send_ok(lua_State *L);
static int l_client_send_prev(lua_State *L);
static int l_client_send_prev_next(lua_State *L);
static int l_client_send_yes_no(lua_State *L);
static int l_client_message(lua_State *L);
static int l_client_set_hp(lua_State *L);
static int l_client_set_mp(lua_State *L);
static int l_client_set_quest_info(lua_State *L);
static int l_client_start_quest_now(lua_State *L);
static int l_client_to_string(lua_State *L);
static int l_client_warp(lua_State *L);
static int l_client_reset_stats(lua_State *L);
static int l_client_open_storage(lua_State *L);
static int l_client_show_info(lua_State *L);
static int l_client_show_intro(lua_State *L);

static const struct luaL_Reg clientlib[] = {
    { "activeNpc", l_client_active_npc },
    { "changeJob", l_client_change_job },
    { "enableActions", l_client_enable_actions },
    { "endQuestNow", l_client_end_quest_now },
    { "gainExp", l_client_gain_exp },
    { "gainFame", l_client_gain_fame },
    { "gainHp", l_client_gain_hp },
    { "gainItems", l_client_gain_items },
    { "gainMeso", l_client_gain_meso },
    { "gainMp", l_client_gain_mp },
    { "gender", l_client_gender },
    { "hasItem", l_client_has_item },
    { "hp", l_client_hp },
    { "isQuestStarted", l_client_is_quest_started },
    { "isQuestComplete", l_client_is_quest_complete },
    { "job", l_client_job },
    { "level", l_client_level },
    { "maxHp", l_client_max_hp },
    { "maxMp", l_client_max_mp },
    { "meso", l_client_meso },
    { "mp", l_client_mp },
    { "openShop", l_client_open_shop },
    { "resetStats", l_client_reset_stats },
    { "sendAcceptDecline", l_client_send_accept_decline },
    { "sendSimple", l_client_send_simple },
    { "sendNext", l_client_send_next },
    { "sendOk", l_client_send_ok },
    { "sendPrev", l_client_send_prev },
    { "sendPrevNext", l_client_send_prev_next },
    { "sendYesNo", l_client_send_yes_no },
    { "message", l_client_message },
    { "setHp", l_client_set_hp },
    { "setMp", l_client_set_mp },
    { "setQuestInfo", l_client_set_quest_info },
    { "startQuestNow", l_client_start_quest_now },
    { "warp", l_client_warp },
    { "openStorage", l_client_open_storage },
    { "showInfo", l_client_show_info },
    { "showIntro", l_client_show_intro },
    { "__tostring", l_client_to_string },
    { NULL, NULL }
};
int luaopen_client(lua_State *L)
{
    luaL_newmetatable(L, SCRIPT_CLIENT_TYPE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, clientlib, 0);
    return 1;
}

static int l_client_active_npc(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_active_npc(client));
    return 1;
}

static int l_client_enable_actions(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    client_enable_actions(client);
    return 0;
}

static int l_client_end_quest_now(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    bool success;
    if (!client_end_quest_now(client, &success))
        return luaL_error(L, "Memory error");

    lua_pushboolean(L, success);
    return 1;
}

static int l_client_change_job(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int16_t job = luaL_checkinteger(L, 2);
    client_change_job(client, job);
    client_commit_stats(client);
    return 0;
}

static int l_client_max_hp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->maxHp);
    return 1;
}

static int l_client_hp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->hp);
    return 1;
}

static int l_client_set_hp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int16_t hp = luaL_checkinteger(L, 2);
    client_set_hp(client, hp);
    return 0;
}

static int l_client_gain_hp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int16_t hp = luaL_checkinteger(L, 2);
    client_adjust_hp_now(client, hp);
    return 0;
}

static int l_client_max_mp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->maxMp);
    return 1;
}

static int l_client_mp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->mp);
    return 1;
}

static int l_client_open_shop(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint32_t id = luaL_checkinteger(L, 2);
    client_open_shop(client, id);
    return 0;
}

static int l_client_set_mp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int16_t mp = luaL_checkinteger(L, 2);
    client_set_mp(client, mp);
    return 0;
}

static int l_client_gain_mp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int16_t mp = luaL_checkinteger(L, 2);
    client_adjust_mp_now(client, mp);
    return 0;
}

static int l_client_job(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->job);
    return 1;
}

static int l_client_gender(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->gender);
    return 1;
}

static int l_client_has_item(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint32_t id = luaL_checkinteger(L, 2);
    lua_pushboolean(L, client_has_item(client, id));
    return 1;
}

static int l_client_gain_items(lua_State *L) {
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (luaL_len(L, 2) == 0)
        return 0;

    uint32_t ids[luaL_len(L, 2)];
    int16_t amounts[luaL_len(L, 2)];
    for (int i = 1; i <= luaL_len(L, 2); i++) {
        int type = lua_geti(L, 2, i);
        if (type != LUA_TTABLE)
            return luaL_error(L, "adjustItems(): Expected an array of tables");

        lua_pushstring(L, "id");
        if (lua_gettable(L, 3) != LUA_TNUMBER) {
            lua_pushstring(L, "id");
            fprintf(stderr, "%d\n", lua_gettable(L, 3));
            return luaL_error(L, "adjustItems(): id field in table is not an integer");
        }
        ids[i - 1] = luaL_checkinteger(L, -1);
        lua_pop(L, 1);

        lua_pushstring(L, "amount");
        if (lua_gettable(L, 3) != LUA_TNUMBER)
            return luaL_error(L, "adjustItems(): amount field in table is not an integer");
        amounts[i - 1] = luaL_checkinteger(L, -1);
        lua_pop(L, 2);
    }
    bool success;
    if (!client_gain_items(client, luaL_len(L, 2), ids, amounts, true, &success))
        return luaL_error(L, "adjustItems(): Memory error");
    lua_pushboolean(L, success);
    return 1;
}

static int l_client_gain_meso(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int32_t amount = luaL_checkinteger(L, 2);
    client_gain_meso(client, amount, false, true);
    client_commit_stats(client);
    return 0;
}

static int l_client_gain_exp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int32_t exp = luaL_checkinteger(L, 2);
    client_gain_exp(client, exp, true);
    client_commit_stats(client);
    return 0;
}

static int l_client_gain_fame(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    int16_t fame = luaL_checkinteger(L, 2);
    client_adjust_fame(client, fame);
    client_commit_stats(client);
    return 0;
}

static int l_client_is_quest_started(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint16_t qid = luaL_checkinteger(L, 2);
    lua_pushboolean(L, client_is_quest_started(client, qid));
    return 1;
}

static int l_client_is_quest_complete(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint16_t qid = luaL_checkinteger(L, 2);
    lua_pushboolean(L, client_is_quest_complete(client, qid));
    return 1;
}

static int l_client_level(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->level);
    return 1;
}

static int l_client_meso(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushinteger(L, client_get_character(client)->mesos);
    return 1;
}

static int l_client_set_quest_info(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint16_t info = luaL_checkinteger(L, 2);
    const char *value = luaL_checkstring(L, 3);
    if (client_set_quest_info(client, info, value) == -1)
        return luaL_error(L, "Memory error");
    return 0;
}

static int l_client_start_quest_now(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    bool success;
    if (!client_start_quest_now(client, &success))
        return luaL_error(L, "Memory error");

    lua_pushboolean(L, success);
    return 1;
}

static int l_client_send_ok(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint8_t speaker = lua_isinteger(L, 2) ? lua_tointeger(L, 2) : 0;
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_send_ok(client, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_client_send_next(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint8_t speaker = lua_isinteger(L, 2) ? lua_tointeger(L, 2) : 0;
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_send_next(client, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_client_send_prev_next(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint8_t speaker = lua_isinteger(L, 2) ? lua_tointeger(L, 2) : 0;
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_send_prev_next(client, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_client_send_prev(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint8_t speaker = lua_isinteger(L, 2) ? lua_tointeger(L, 2) : 0;
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_send_prev(client, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_client_send_yes_no(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint8_t speaker = lua_isinteger(L, 2) ? lua_tointeger(L, 2) : 0;
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_send_yes_no(client, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_client_message(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_message(client, str);
    return 1;
}

static int l_client_send_accept_decline(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint8_t speaker = lua_isinteger(L, 2) ? lua_tointeger(L, 2) : 0;
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_send_accept_decline(client, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_client_send_simple(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint8_t speaker = lua_isinteger(L, 2) ? lua_tointeger(L, 2) : 0;
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    client_send_simple(client, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_client_warp(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    uint32_t map = luaL_checkinteger(L, 2);
    uint8_t portal = luaL_checkinteger(L, 3);
    client_warp(client, map, portal);
    return lua_yield(L, 2);
}

static int l_client_reset_stats(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    client_reset_stats(client);
    return 0;
}

static int l_client_open_storage(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    client_open_storage(client);
    return 0;
}

static int l_client_show_info(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    client_show_info(client, luaL_checkstring(L, 2));
    return 0;
}

static int l_client_show_intro(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    client_show_intro(client, luaL_checkstring(L, 2));
    return 0;
}

static int l_client_to_string(lua_State *L)
{
    struct Client *client = *(void **)luaL_checkudata(L, 1, SCRIPT_CLIENT_TYPE);
    lua_pushfstring(L, "client: %p", client);
    return 1;
}



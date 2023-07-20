#include "user.h"

#include <lauxlib.h>

#include "../user.h"
#include "script-manager.h"

static int l_user_active_npc(lua_State *L);
static int l_user_active_quest(lua_State *L);
static int l_user_enable_actions(lua_State *L);
static int l_user_end_quest_now(lua_State *L);
static int l_user_change_job(lua_State *L);
static int l_user_gain_exp(lua_State *L);
static int l_user_gain_fame(lua_State *L);
static int l_user_gain_hp(lua_State *L);
static int l_user_gain_items(lua_State *L);
static int l_user_gain_meso(lua_State *L);
static int l_user_gain_mp(lua_State *L);
static int l_user_gender(lua_State *L);
static int l_user_hp(lua_State *L);
static int l_user_max_hp(lua_State *L);
static int l_user_max_mp(lua_State *L);
static int l_user_mp(lua_State *L);
static int l_user_open_shop(lua_State *L);
static int l_user_job(lua_State *L);
static int l_user_has_item(lua_State *L);
static int l_user_is_quest_started(lua_State *L);
static int l_user_is_quest_complete(lua_State *L);
static int l_user_level(lua_State *L);
static int l_user_meso(lua_State *L);
static int l_user_send_accept_decline(lua_State *L);
static int l_user_send_simple(lua_State *L);
static int l_user_send_next(lua_State *L);
static int l_user_send_ok(lua_State *L);
static int l_user_send_prev(lua_State *L);
static int l_user_send_prev_next(lua_State *L);
static int l_user_send_yes_no(lua_State *L);
static int l_user_send_get_number(lua_State *L);
static int l_user_message(lua_State *L);
static int l_user_map(lua_State *L);
static int l_user_set_hp(lua_State *L);
static int l_user_set_mp(lua_State *L);
static int l_user_get_quest_info(lua_State *L);
static int l_user_set_quest_info(lua_State *L);
static int l_user_start_quest_now(lua_State *L);
static int l_user_to_string(lua_State *L);
static int l_user_warp(lua_State *L);
static int l_user_reset_stats(lua_State *L);
static int l_user_open_storage(lua_State *L);
static int l_user_show_info(lua_State *L);
static int l_user_show_intro(lua_State *L);

static const struct luaL_Reg userlib[] = {
    { "activeNpc", l_user_active_npc },
    { "activeQuest", l_user_active_quest },
    { "changeJob", l_user_change_job },
    { "enableActions", l_user_enable_actions },
    { "endQuestNow", l_user_end_quest_now },
    { "gainExp", l_user_gain_exp },
    { "gainFame", l_user_gain_fame },
    { "gainHp", l_user_gain_hp },
    { "gainItems", l_user_gain_items },
    { "gainMeso", l_user_gain_meso },
    { "gainMp", l_user_gain_mp },
    { "gender", l_user_gender },
    { "hasItem", l_user_has_item },
    { "hp", l_user_hp },
    { "isQuestStarted", l_user_is_quest_started },
    { "isQuestComplete", l_user_is_quest_complete },
    { "job", l_user_job },
    { "level", l_user_level },
    { "maxHp", l_user_max_hp },
    { "maxMp", l_user_max_mp },
    { "meso", l_user_meso },
    { "mp", l_user_mp },
    { "openShop", l_user_open_shop },
    { "resetStats", l_user_reset_stats },
    { "sendAcceptDecline", l_user_send_accept_decline },
    { "sendSimple", l_user_send_simple },
    { "sendNext", l_user_send_next },
    { "sendOk", l_user_send_ok },
    { "sendPrev", l_user_send_prev },
    { "sendPrevNext", l_user_send_prev_next },
    { "sendYesNo", l_user_send_yes_no },
    { "sendGetNumber", l_user_send_get_number },
    { "map", l_user_map },
    { "message", l_user_message },
    { "setHp", l_user_set_hp },
    { "setMp", l_user_set_mp },
    { "getQuestInfo", l_user_get_quest_info },
    { "setQuestInfo", l_user_set_quest_info },
    { "startQuestNow", l_user_start_quest_now },
    { "warp", l_user_warp },
    { "openStorage", l_user_open_storage },
    { "showInfo", l_user_show_info },
    { "showIntro", l_user_show_intro },
    { "__tostring", l_user_to_string },
    { NULL, NULL }
};

int luaopen_user(lua_State *L)
{
    luaL_newmetatable(L, SCRIPT_USER_TYPE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, userlib, 0);
    return 1;
}

static int l_user_active_npc(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_active_npc(user));
    return 1;*/
    return 0;
}

static int l_user_active_quest(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_active_quest(user));*/
    return 0;
}

static int l_user_enable_actions(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    user_enable_actions(user);
    return 0;
}

static int l_user_end_quest_now(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    if (!user_end_quest_now(user))
        return luaL_error(L, "Memory error");
    return 0;
}

static int l_user_change_job(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int16_t job = luaL_checkinteger(L, 2);
    user_change_job(user, job);*/
    return 0;
}

static int l_user_max_hp(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->maxHp);*/
    return 0;
}

static int l_user_hp(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->hp);
    return 1;
}

static int l_user_set_hp(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int16_t hp = luaL_checkinteger(L, 2);
    user_set_hp(user, hp);
    return 0;
}

static int l_user_gain_hp(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int16_t hp = luaL_checkinteger(L, 2);
    user_adjust_hp(user, hp);*/
    return 0;
}

static int l_user_max_mp(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->maxMp);*/
    return 0;
}

static int l_user_mp(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->mp);*/
    return 0;
}

static int l_user_open_shop(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    uint32_t id = luaL_checkinteger(L, 2);
    user_open_shop(user, id);*/
    return 0;
}

static int l_user_set_mp(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int16_t mp = luaL_checkinteger(L, 2);
    user_set_mp(user, mp);*/
    return 0;
}

static int l_user_gain_mp(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int16_t mp = luaL_checkinteger(L, 2);
    user_adjust_mp(user, mp);*/
    return 0;
}

static int l_user_job(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->job);*/
    return 0;
}

static int l_user_gender(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->gender);*/
    return 0;
}

static int l_user_has_item(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    uint32_t id = luaL_checkinteger(L, 2);
    int16_t qty = lua_isinteger(L, 3) ? lua_tointeger(L, 3) : 1;
    lua_pushboolean(L, user_has_item(user, id, qty));
    return 1;
}

static int l_user_gain_items(lua_State *L) {
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    luaL_checktype(L, 2, LUA_TTABLE);

    if (luaL_len(L, 2) != 0) {
        uint32_t ids[luaL_len(L, 2)];
        int16_t amounts[luaL_len(L, 2)];
        for (int i = 1; i <= luaL_len(L, 2); i++) {
            int type = lua_geti(L, 2, i);
            if (type == LUA_TTABLE) {
                lua_pushstring(L, "id");
                if (lua_gettable(L, 3) != LUA_TNUMBER)
                    return luaL_error(L, "gainItems(): id field in table is not an integer");

                ids[i - 1] = lua_tointeger(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "amount");
                if (lua_gettable(L, 3) != LUA_TNUMBER)
                    return luaL_error(L, "gainItems(): amount field in table is not an integer");
                amounts[i - 1] = lua_tointeger(L, -1);
                lua_pop(L, 2);
            } else {
                return luaL_error(L, "gainItems(): Expected an array of tables or a single table");
            }

        }

        bool success;
        if (!user_gain_items(user, luaL_len(L, 2), ids, amounts, true, &success))
            return luaL_error(L, "gainItems(): Memory error");
        lua_pushboolean(L, success);
    } else {
        uint32_t id;
        int16_t amount;
        lua_pushstring(L, "id");
        if (lua_gettable(L, 2) != LUA_TNUMBER)
            return luaL_error(L, "gainItems(): id field in table is not an integer");

        id = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_pushstring(L, "amount");
        int type = lua_gettable(L, 2);
        if (type != LUA_TNUMBER && type != LUA_TNIL)
            return luaL_error(L, "gainItems(): amount field in table is not an integer");
        amount = type == LUA_TNIL ? 1 : lua_tointeger(L, -1);

        lua_pop(L, 1);
        bool success;
        if (!user_gain_items(user, 1, &id, &amount, true, &success))
            return luaL_error(L, "gainItems(): Memory error");
        lua_pushboolean(L, success);
    }
    return 1;
}

static int l_user_gain_meso(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int32_t amount = luaL_checkinteger(L, 2);
    user_gain_meso(user, amount, false, true);
    return 0;
}

static int l_user_gain_exp(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int32_t exp = luaL_checkinteger(L, 2);
    user_gain_exp(user, exp, true, NULL); // TODO: Broadcast level up to room
    return 0;
}

static int l_user_gain_fame(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    int16_t fame = luaL_checkinteger(L, 2);
    user_adjust_fame(user, fame);*/
    return 0;
}

static int l_user_is_quest_started(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    uint16_t qid = luaL_checkinteger(L, 2);
    lua_pushboolean(L, user_is_quest_started(user, qid));*/
    return 1;
}

static int l_user_is_quest_complete(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    uint16_t qid = luaL_checkinteger(L, 2);
    lua_pushboolean(L, user_is_quest_complete(user, qid));*/
    return 1;
}

static int l_user_level(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->level);*/
    return 0;
}

static int l_user_meso(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_meso(user));
    return 0;
}

static int l_user_get_quest_info(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    uint16_t info = luaL_checkinteger(L, 2);
    lua_pushstring(L, user_get_quest_info(user, info));
    return 0;
}

static int l_user_set_quest_info(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    uint16_t info = luaL_checkinteger(L, 2);
    const char *value = luaL_checkstring(L, 3);
    if (user_set_quest_info(user, info, value) == -1)
        return luaL_error(L, "Memory error");
    return 0;
}

static int l_user_start_quest_now(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    bool success;
    if (!user_start_quest_now(user, &success))
        return luaL_error(L, "Memory error");

    lua_pushboolean(L, success);
    return 1;
}

static int l_user_send_ok(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    uint8_t speaker = lua_isinteger(L, 3) ? lua_tointeger(L, 3) : 0;
    user_send_ok(user, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_user_send_next(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    uint8_t speaker = lua_isinteger(L, 3) ? lua_tointeger(L, 3) : 0;
    user_send_next(user, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_user_send_prev_next(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    uint8_t speaker = lua_isinteger(L, 3) ? lua_tointeger(L, 3) : 0;
    user_send_prev_next(user, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_user_send_prev(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    uint8_t speaker = lua_isinteger(L, 3) ? lua_tointeger(L, 3) : 0;
    user_send_prev(user, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_user_send_yes_no(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    uint8_t speaker = lua_isinteger(L, 3) ? lua_tointeger(L, 3) : 0;
    user_send_yes_no(user, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_user_send_get_number(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    int32_t def = luaL_checkinteger(L, 3);
    int32_t min = luaL_checkinteger(L, 4);
    int32_t max = luaL_checkinteger(L, 5);
    uint8_t speaker = lua_isinteger(L, 6) ? lua_tointeger(L, 6) : 0;
    user_send_get_number(user, len, str, speaker, def, min, max);
    return lua_yield(L, 0);
}

static int l_user_map(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushinteger(L, user_get_character(user)->map);*/
    return 0;
}

static int l_user_message(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    user_message(user, str);
    return 0;
}

static int l_user_send_accept_decline(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    uint8_t speaker = lua_isinteger(L, 3) ? lua_tointeger(L, 3) : 0;
    user_send_accept_decline(user, len, str, speaker);
    return lua_yield(L, 0);
}

static int l_user_send_simple(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    size_t len;
    const char *str = luaL_checklstring(L, 2, &len);
    uint8_t speaker = lua_isinteger(L, 4) ? lua_tointeger(L, 4) : 0;
    user_send_simple(user, len, str, speaker, lua_tointeger(L, 3));
    return lua_yield(L, 0);
}

static int l_user_warp(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    uint32_t map = luaL_checkinteger(L, 2);
    uint8_t portal = luaL_checkinteger(L, 3);
    user_warp(user, map, portal);*/
    return lua_yield(L, 0);
}

static int l_user_reset_stats(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    user_reset_stats(user);*/
    return 0;
}

static int l_user_open_storage(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    user_open_storage(user);*/
    return 0;
}

static int l_user_show_info(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    user_show_info(user, luaL_checkstring(L, 2));
    return 0;
}

static int l_user_show_intro(lua_State *L)
{
    /*struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    user_show_intro(user, luaL_checkstring(L, 2));*/
    return 0;
}

static int l_user_to_string(lua_State *L)
{
    struct User *user = *(void **)luaL_checkudata(L, 1, SCRIPT_USER_TYPE);
    lua_pushfstring(L, "user: %p", user);
    return 1;
}



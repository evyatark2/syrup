#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "script-manager.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "client.h"
#include "job.h"
#include "reactor-manager.h"

struct Script {
    char *name;
    mtx_t mtx;
    lua_State *L;
};

struct ScriptManager {
    struct Script def;
    size_t scriptCount;
    struct Script *scripts;
    size_t entryPointCount;
    struct ScriptEntryPoint *entryPoints;
};

struct ScriptInstance {
    lua_State *L;
    struct Script *script;
    struct ScriptEntryPoint *entry;
    bool started;
};

struct ScriptManager *script_manager_create(const char *dir_name, const char *def, size_t entry_point_count, struct ScriptEntryPoint *entry_points)
{
    struct ScriptManager *sm = malloc(sizeof(struct ScriptManager));
    if (sm == NULL)
        return NULL;

    int dirfd = open(dir_name, O_RDONLY | O_DIRECTORY);
    if (dirfd == -1) {
        free(sm);
        return NULL;
    }

    DIR *dir = fdopendir(dirfd);
    struct dirent *ent;
    sm->scriptCount = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG && ent->d_name[0] != '.' && strcmp(ent->d_name, def)) {
            sm->scriptCount++;
        } else if (ent->d_type == DT_UNKNOWN) {
            struct stat stat;
            fstatat(dirfd, ent->d_name, &stat, 0);
            if (stat.st_mode & S_IFREG && strcmp(ent->d_name, def))
                sm->scriptCount++;
        }
    }
    sm->scripts = malloc(sm->scriptCount * sizeof(struct Script));
    if (sm->scripts == NULL) {
        closedir(dir);
        free(sm);
        return NULL;
    }

    rewinddir(dir);
    size_t i = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        int fd = openat(dirfd, ent->d_name, O_RDONLY);
        if (fd == -1)
            continue;

        struct stat stat;
        fstat(fd, &stat);
        if (!(stat.st_mode & S_IFREG)) {
            close(fd);
            continue;
        }

        size_t size = lseek(fd, 0, SEEK_END);
        char *buf = malloc(size);
        if (buf == NULL) {
            close(fd);
            continue;
        }

        lseek(fd, 0, SEEK_SET);
        read(fd, buf, size);

        struct Script *script = !strcmp(ent->d_name, def) ? &sm->def : &sm->scripts[i];

        script->name = malloc(strlen(ent->d_name) + 1);
        mtx_init(&script->mtx, mtx_plain);
        script->L = luaL_newstate();
        strcpy(script->name, ent->d_name);
        luaL_openlibs(script->L);
        luaopen_client(script->L);
        luaopen_reactor_manager(script->L);
        luaopen_job(script->L);
        lua_setglobal(script->L, "Job");
        if (luaL_loadbuffer(script->L, buf, size, ent->d_name) != LUA_OK || lua_pcall(script->L, 0, 0, 0))
            fprintf(stderr, "Failed to load %s: %s\n", ent->d_name, lua_tostring(script->L, -1));
        free(buf);
        close(fd);
        if (script != &sm->def)
            i++;
    }
    closedir(dir);

    sm->entryPoints = malloc(entry_point_count * sizeof(struct ScriptEntryPoint));
    if (sm->entryPoints == NULL)
        return NULL; // TODO

    sm->entryPointCount = entry_point_count;
    memcpy(sm->entryPoints, entry_points, entry_point_count * sizeof(struct ScriptEntryPoint));

    return sm;
}

void script_manager_destroy(struct ScriptManager *sm)
{
    for (size_t i = 0; i < sm->scriptCount; i++) {
        lua_close(sm->scripts[i].L);
        mtx_destroy(&sm->scripts[i].mtx);
        free(sm->scripts[i].name);
    }
    free(sm->scripts);
    lua_close(sm->def.L);
    mtx_destroy(&sm->def.mtx);
    free(sm->def.name);
    free(sm->entryPoints);
    free(sm);
}

struct ScriptInstance *script_manager_alloc(struct ScriptManager *sm, const char *name, size_t entry)
{
    struct ScriptInstance *handle = malloc(sizeof(struct ScriptInstance));
    handle->L = NULL;
    for (size_t i = 0; i < sm->scriptCount; i++) {
        if (strcmp(sm->scripts[i].name, name) == 0) {
            mtx_lock(&sm->scripts[i].mtx);
            handle->L = lua_newthread(sm->scripts[i].L);
            if (handle->L == NULL) {
                mtx_unlock(&sm->scripts[i].mtx);
                free(handle);
                return NULL;
            }

            mtx_unlock(&sm->scripts[i].mtx);
            handle->script = &sm->scripts[i];
            break;
        }
    }

    if (handle->L == NULL) {
        mtx_lock(&sm->def.mtx);
        handle->L = lua_newthread(sm->def.L);
        if (handle->L == NULL) {
            mtx_unlock(&sm->def.mtx);
            free(handle);
            return NULL;
        }

        mtx_unlock(&sm->def.mtx);
        handle->script = &sm->def;
    }

    handle->entry = &sm->entryPoints[entry];
    handle->started = false;

    return handle;
}

struct ScriptResult script_manager_run(struct ScriptInstance *handle, ...)
{
    va_list list;
    va_start(list, handle);
    if (!handle->started) {
        handle->started = true;
        lua_getglobal(handle->L, handle->entry->name);
        for (size_t i = 0; i < handle->entry->argCount; i++) {
            switch (handle->entry->args[i]) {
            case SCRIPT_VALUE_TYPE_BOOLEAN:
                lua_pushboolean(handle->L, va_arg(list, int));
            break;

            case SCRIPT_VALUE_TYPE_INTEGER:
                lua_pushinteger(handle->L, va_arg(list, int));
            break;

            case SCRIPT_VALUE_TYPE_USERDATA: {
                char *type = va_arg(list, char *);
                void **data = lua_newuserdata(handle->L, sizeof(void *));
                *data = va_arg(list, void *);
                luaL_getmetatable(handle->L, type);
                lua_setmetatable(handle->L, -2);
            }
            break;
            }
        }
        va_end(list);
        int results;
        int res = lua_resume(handle->L, NULL, handle->entry->argCount, &results);
        if (res == LUA_OK) {
            union ScriptValue value;
            switch (handle->entry->result) {
            case SCRIPT_VALUE_TYPE_BOOLEAN:
                value.b = lua_toboolean(handle->L, -1);
            break;

            case SCRIPT_VALUE_TYPE_INTEGER:
                value.i = lua_tointeger(handle->L, -1);
            break;
            }
            lua_pop(handle->L, results);
            return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_SUCCESS, .value = value };
        } else if (res == LUA_YIELD) {
            // this is a ridicolous way to do this but for now it works
            if (results == 2) {
                struct ScriptResult res = {
                    .result = SCRIPT_RESULT_VALUE_WARP,
                    .value.i = lua_tointeger(handle->L, -2),
                    .value2.i = lua_tointeger(handle->L, -1),
                };
                lua_pop(handle->L, results);
                return res;
            } else if (results == 1) {
                return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_KICK };
            } else {
                return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_NEXT };
            }
        } else {
            fprintf(stderr, "Lua error: %s\n", lua_tostring(handle->L, -1));
            lua_pop(handle->L, results);
            return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_FAILURE };
        }
    } else {
        // For now, an yield can only return an integer
        lua_pushinteger(handle->L, va_arg(list, int));
        va_end(list);
        int results;
        int res = lua_resume(handle->L, NULL, 1, &results);
        if (res == LUA_OK) {
            union ScriptValue value;
            switch (handle->entry->result) {
            case SCRIPT_VALUE_TYPE_BOOLEAN:
                value.b = lua_toboolean(handle->L, -1);
            break;

            case SCRIPT_VALUE_TYPE_INTEGER:
                value.i = lua_tointeger(handle->L, -1);
            break;
            }
            lua_pop(handle->L, results);
            return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_SUCCESS, .value = value };
        } else if (res == LUA_YIELD) {
            // this is a ridicolous way to do this but for now it works
            if (results == 2) {
                struct ScriptResult res = {
                    .result = SCRIPT_RESULT_VALUE_WARP,
                    .value.i = lua_tointeger(handle->L, -2),
                    .value2.i = lua_tointeger(handle->L, -1),
                };
                lua_pop(handle->L, results);
                return res;
            } else if (results == 1) {
                return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_KICK };
            } else {
                return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_NEXT };
            }
        } else {
            fprintf(stderr, "Lua error: %s\n", lua_tostring(handle->L, -1));
            lua_pop(handle->L, results);
            return (struct ScriptResult) { .result = SCRIPT_RESULT_VALUE_FAILURE };
        }
    }
}

void script_manager_free(struct ScriptInstance *handle)
{
    if (handle != NULL) {
        lua_State *L = handle->script->L;
        mtx_lock(&handle->script->mtx);
        for (size_t i = 1; i <= lua_gettop(L); i++) {
            lua_State *L1 = lua_tothread(L, i);
            if (L1 == handle->L) {
                lua_remove(L, i);
                break;
            }
        }
        mtx_unlock(&handle->script->mtx);
    }
    free(handle);
}


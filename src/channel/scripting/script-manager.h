#ifndef SCRIPT_MANAGER_H
#define SCRIPT_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ScriptValueType {
    SCRIPT_VALUE_TYPE_VOID,
    SCRIPT_VALUE_TYPE_BOOLEAN,
    SCRIPT_VALUE_TYPE_INTEGER,
    SCRIPT_VALUE_TYPE_USERDATA,
};

struct ScriptEntryPoint {
    char *name;
    size_t argCount;
    enum ScriptValueType *args;
};

struct ScriptManager;
struct ScriptInstance;

struct NpcScriptManager;
struct PortalScriptManager;
struct QuestScriptManager;
struct ReactorScriptManager;

enum ScriptResult {
    SCRIPT_RESULT_VALUE_KICK = -2,
    SCRIPT_RESULT_VALUE_FAILURE,
    SCRIPT_RESULT_VALUE_SUCCESS,
    SCRIPT_RESULT_VALUE_NEXT,
};

struct ScriptManager *script_manager_create(const char *dir_name, const char *def, size_t entry_point_count, struct ScriptEntryPoint *entry_points);
void script_manager_destroy(struct ScriptManager *manager);
struct ScriptInstance *script_manager_alloc(struct ScriptManager *manager, const char *name, size_t entry);
enum ScriptResult script_manager_run(struct ScriptInstance *handle, ...);
void script_manager_free(struct ScriptInstance *handle);

#endif


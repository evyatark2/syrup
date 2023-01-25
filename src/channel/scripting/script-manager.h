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
    enum ScriptValueType result;
};

struct ScriptManager;
struct ScriptHandle;

enum ScriptResultValue {
    SCRIPT_RESULT_VALUE_KICK = -2,
    SCRIPT_RESULT_VALUE_FAILURE,
    SCRIPT_RESULT_VALUE_SUCCESS,
    SCRIPT_RESULT_VALUE_NEXT,
    SCRIPT_RESULT_VALUE_WARP,
};

union ScriptValue {
    bool b;
    int32_t i;
};

struct ScriptResult {
    enum ScriptResultValue result;
    union ScriptValue value;
    union ScriptValue value2;
};

struct ScriptManager *script_manager_create(const char *dir_name, const char *def, size_t entry_point_count, struct ScriptEntryPoint *entry_points);
void script_manager_destroy(struct ScriptManager *manager);
struct ScriptHandle *script_manager_alloc(struct ScriptManager *manager, const char *name, size_t entry);
struct ScriptResult script_manager_run(struct ScriptHandle *handle, ...);
void script_manager_free(struct ScriptHandle *handle);

#endif


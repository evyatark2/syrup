#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json.h>

struct ChannelConfig CHANNEL_CONFIG;

static bool json_get(json_object *obj, const char *key, json_object **value, enum json_type type);

json_object *ROOT;

#define JSON_GET(src, key, dst, type) \
        do { \
            if (!json_get(src, key, dst, type)) { \
                json_object_put(ROOT); \
                return -1; \
            } \
        } while (false)

#define JSON_GET_BOOL(src, key, value) JSON_GET(src, key, value, json_type_boolean)
#define JSON_GET_INT(src, key, value) JSON_GET(src, key, value, json_type_int)
#define JSON_GET_DOUBLE(src, key, value) JSON_GET(src, key, value, json_type_double)
#define JSON_GET_STRING(src, key, value) JSON_GET(src, key, value, json_type_string)
#define JSON_GET_OBJECT(src, key, value) JSON_GET(src, key, value, json_type_object)
#define JSON_GET_ARRAY(src, key, value) JSON_GET(src, key, value, json_type_array)

int channel_config_load(const char *file_name)
{
    FILE *file = fopen(file_name, "r");
    if (file == NULL)
        return -1;

    if (fseek(file, 0, SEEK_END) == -1) {
        fclose(file);
        return -1;
    }

    size_t len = ftell(file);
    if (len == -1) {
        fclose(file);
        return -1;
    }

    rewind(file);

    void *data = malloc(len);
    if (data == NULL) {
        fclose(file);
        return -1;
    }

    if (fread(data, 1, len, file) < len) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);

    struct json_tokener *tokener = json_tokener_new();
    if (tokener == NULL) {
        free(data);
        return -1;
    }

    ROOT = json_tokener_parse_ex(tokener, data, len);
    free(data);
    json_tokener_free(tokener);
    if (ROOT == NULL)
        return -1;

    if (json_object_get_type(ROOT) != json_type_object) {
        json_object_put(ROOT);
        return -1;
    }

    json_object *database;
    JSON_GET_OBJECT(ROOT, "database", &database);

    json_object *host;
    JSON_GET_STRING(database, "host", &host);
    CHANNEL_CONFIG.database.host = json_object_get_string(host);

    json_object *port;
    if (json_object_object_get_ex(database, "port", &port)) {
        if (json_object_get_type(port) != json_type_int) {
            json_object_put(ROOT);
            return -1;
        }

        if (json_object_get_int(port) < 0 || json_object_get_int(port) > 65535) {
            json_object_put(ROOT);
            return -1;
        }

        CHANNEL_CONFIG.database.port = json_object_get_int(port);
    } else {
        CHANNEL_CONFIG.database.port = 0;
    }

    json_object *user;
    JSON_GET_STRING(database, "user", &user);
    CHANNEL_CONFIG.database.user = json_object_get_string(user);

    json_object *password;
    if (json_object_object_get_ex(database, "password", &password)) {
        if (json_object_get_type(password) != json_type_string) {
            json_object_put(ROOT);
            return -1;
        }

        CHANNEL_CONFIG.database.password = json_object_get_string(password);
    } else {
        CHANNEL_CONFIG.database.password = NULL;
    }

    json_object *db;
    JSON_GET_STRING(database, "db", &db);
    CHANNEL_CONFIG.database.db = json_object_get_string(db);

    json_object *listen;
    JSON_GET_STRING(ROOT, "listen", &listen);
    CHANNEL_CONFIG.listen = json_object_get_string(listen);

    return 0;
}

void channel_config_unload()
{
    json_object_put(ROOT);
}

static bool json_get(json_object *obj, const char *key, json_object **value, enum json_type type)
{
    if (!json_object_object_get_ex(obj, key, value) || json_object_get_type(*value) != type)
        return false;

    return true;
}


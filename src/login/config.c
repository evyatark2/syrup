#include "config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>

#include <json.h>

json_object *ROOT;
struct LoginConfig LOGIN_CONFIG;

static bool json_get(json_object *obj, const char *key, json_object **value, enum json_type type);

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

int login_config_load(const char *file_name)
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
    LOGIN_CONFIG.database.host = json_object_get_string(host);

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

        LOGIN_CONFIG.database.port = json_object_get_int(port);
    } else {
        LOGIN_CONFIG.database.port = 0;
    }

    json_object *user;
    JSON_GET_STRING(database, "user", &user);
    LOGIN_CONFIG.database.user = json_object_get_string(user);

    json_object *password;
    if (json_object_object_get_ex(database, "password", &password)) {
        if (json_object_get_type(password) != json_type_string) {
            json_object_put(ROOT);
            return -1;
        }

        LOGIN_CONFIG.database.password = json_object_get_string(password);
    } else {
        LOGIN_CONFIG.database.password = NULL;
    }

    json_object *db;
    JSON_GET_STRING(database, "db", &db);
    LOGIN_CONFIG.database.db = json_object_get_string(db);

    json_object *worlds;
    JSON_GET_ARRAY(ROOT, "worlds", &worlds);
    if (json_object_array_length(worlds) > 21) {
        json_object_put(ROOT);
        return -1;
    }

    for (uint8_t i = 0; i < json_object_array_length(worlds); i++) {
        json_object *world = json_object_array_get_idx(worlds, i);
        if (json_object_get_type(world) != json_type_object) {
            json_object_put(ROOT);
            return -1;
        }

        json_object *channels;
        JSON_GET_ARRAY(world, "channels", &channels);

        for (uint8_t j = 0; j < json_object_array_length(channels); j++) {
            json_object *channel = json_object_array_get_idx(channels, j);
            if (json_object_get_type(channel) != json_type_object) {
                json_object_put(ROOT);
                return -1;
            }

            json_object *host;
            JSON_GET_STRING(channel, "host", &host);
            LOGIN_CONFIG.worlds[i].channels[j].host = json_object_get_string(host);

            json_object *ip;
            JSON_GET_STRING(channel, "ip", &ip);
            if (inet_pton(AF_INET, json_object_get_string(ip), &LOGIN_CONFIG.worlds[i].channels[j].ip) != 1) {
                json_object_put(ROOT);
                return -1;
            }

            json_object *port;
            JSON_GET_INT(channel, "port", &port);
            if (json_object_get_int(port) < 0 || json_object_get_int(port) > 65535) {
                json_object_put(ROOT);
                return -1;
            }

            LOGIN_CONFIG.worlds[i].channels[j].port = json_object_get_int(port);
        }

        LOGIN_CONFIG.worlds[i].channelCount = json_object_array_length(channels);
    }

    LOGIN_CONFIG.worldCount = json_object_array_length(worlds);

    return 0;
}

void login_config_unload()
{
    json_object_put(ROOT);
}

static bool json_get(json_object *obj, const char *key, json_object **value, enum json_type type)
{
    if (!json_object_object_get_ex(obj, key, value) || json_object_get_type(*value) != type)
        return false;

    return true;
}


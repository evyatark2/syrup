#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

struct LoginConfig {
    struct {
        const char *host;
        uint16_t port;
        const char *user;
        const char *password;
        const char *db;
    } database;
    uint8_t worldCount;
    struct {
        uint8_t channelCount;
        struct {
            const char *host;
            uint32_t ip;
            uint16_t port;
        } channels[20];
    } worlds[21];
};

extern struct LoginConfig LOGIN_CONFIG;

int login_config_load(const char *file_name);
void login_config_unload();

#endif


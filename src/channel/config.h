#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

struct ChannelConfig {
    struct {
        const char *host;
        uint16_t port;
        const char *user;
        const char *password;
        const char *db;
    } database;
    const char *listen;
};

extern struct ChannelConfig CHANNEL_CONFIG;

int channel_config_load(const char *file_name);
void channel_config_unload();

#endif


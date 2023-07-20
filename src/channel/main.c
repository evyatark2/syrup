//#define _POSIX_C_SOURCE // For strtok_r
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#include "config.h"
#include "server2.h"

static void on_sigint(int sig);

struct ChannelServer *SERVER;

int main(void)
{
    if (channel_config_load("channel/config.json") == -1)
        return -1;

    SERVER = channel_server_create(7575, CHANNEL_CONFIG.listen);
    if (SERVER == NULL) {
        perror("Channel server creation failed");
        return -1;
    }
    //parties_init();
    //clients_init();

    // Doesn't matter which thread will get the signal
    signal(SIGINT, on_sigint);
    channel_server_start(SERVER);
    channel_server_destroy(SERVER);
    return 0;
}

static void on_sigint(int sig)
{
    channel_server_stop(SERVER);
    signal(sig, SIG_DFL);
}


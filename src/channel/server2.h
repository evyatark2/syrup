#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

struct ChannelServer *channel_server_create(uint16_t port, const char *host);
void channel_server_destroy(struct ChannelServer *);
int channel_server_start(struct ChannelServer *server);
void channel_server_stop(struct ChannelServer *);

#endif


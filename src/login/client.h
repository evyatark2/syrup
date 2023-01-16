#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>

#include "server.h"
#include "../account.h"
#include "../database.h"

enum PacketType {
    PACKET_TYPE_LOGIN,
    PACKET_TYPE_GENDER,
    PACKET_TYPE_CHARACTER_LIST,
    PACKET_TYPE_NAME_CHECK,
    PACKET_TYPE_CREATE_CHARACTER,
    PACKET_TYPE_REGISTER_PIC,
    PACKET_TYPE_VERIFY_PIC,
};

struct Client {
    struct SessionContainer session;
    struct DatabaseConnection *conn;
    enum PacketType type;
    void *handler;
    struct AccountNode *node;
    struct Account account;
    uint8_t world;
    uint8_t channel;
    bool loggedIn;
};

#endif


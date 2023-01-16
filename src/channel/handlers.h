#ifndef LOGIN_HANDLERS_H
#define LOGIN_HANDLERS_H

#include "../packet.h"
#include "map.h"
#include "client.h"

struct LoginHandlerResult {
    int status;
    size_t size;
    uint8_t packet[ENTER_MAP_PACKET_MAX_LENGTH];
};

struct LoginHandler;
struct LoginHandler *login_handler_create(struct Client *client, uint32_t id);
struct LoginHandlerResult login_handler_handle(struct LoginHandler *handler, int status);
void login_handler_destroy(struct LoginHandler *handler);

struct ChangeMapResult {
    size_t size;
    uint8_t packet[CHANGE_MAP_PACKET_LENGTH];
};

struct ChangeMapResult handle_change_map(struct Client *client, uint32_t map, uint8_t target);

struct LogoutHandler;
struct LogoutHandler *logout_handler_create(struct Client *client);
int logout_handler_handle(struct LogoutHandler *handler, int status);
void logout_handler_destroy(struct LogoutHandler *handler);

struct MovePlayerResult {
    size_t size;
    uint8_t packet[MOVE_PLAYER_PACKET_MAX_LENGTH];
};

struct MovePlayerResult handle_move_player(struct Client *client, size_t len, uint8_t *data);

struct DamageMonsterHandler;

struct DamageMonsterHandlerResult {
    int status;
    uint8_t drop_count;
    struct Drop *drops;
    const struct Monster *monster;
};

struct DamageMonsterHandler *damange_monster_handler_create(struct Client *client, const struct Monster *monster);
struct DamageMonsterHandlerResult damange_monster_handler_handle(struct DamageMonsterHandler *handler, int status);
void damange_monster_handler_destroy(struct DamageMonsterHandler *handler);

#endif


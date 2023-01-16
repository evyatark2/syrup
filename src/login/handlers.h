#ifndef LOGIN_HANDLERS_H
#define LOGIN_HANDLERS_H

#include "../packet.h"
#include "client.h"
#include "../hash-map.h"

struct LoginHandlerResult {
    int status;
    size_t size;
    uint8_t packet[LOGIN_SUCCESS_PACKET_MAX_LENGTH];
};

struct LoginHandler;
struct LoginHandler *login_handler_create(struct Client *client, uint8_t name_len, char *name, uint8_t pass_len, char *pass, uint8_t *hwid);
struct LoginHandlerResult login_handler_handle(struct LoginHandler *handler, int status);
void login_handler_destroy(struct LoginHandler *handler);

struct LogoutHandler;
struct LogoutHandler *logout_handler_create(struct Client *client);
int logout_handler_handle(struct LogoutHandler *handler, int status);
void logout_handler_destroy(struct LogoutHandler *handler);

struct TosHandlerResult {
    int status;
    size_t size;
    uint8_t packet[LOGIN_SUCCESS_PACKET_MAX_LENGTH];
};

struct TosHandlerResult handle_tos(struct Client *client_, bool accept);

struct GenderHandler;

struct GenderHandlerResult {
    int status;
    size_t size;
    uint8_t packet[LOGIN_SUCCESS_PACKET_MAX_LENGTH];
};

struct GenderHandlerResult gender_handler_create(struct Client *client, enum AccountGender gender, struct GenderHandler **handler);
struct GenderHandlerResult gender_handler_handle(struct GenderHandler *handler, int status);
void gender_handler_destroy(struct GenderHandler *handler);

struct PinResult {
    int status;
    size_t size;
    uint8_t packet[PIN_PACKET_LENGTH];
};

struct PinResult handle_pin(struct Client *client, uint8_t c2, uint8_t c3);

#define MAX_WORLD_COUNT 21

struct ServerListResult {
    int status;
    size_t worldCount;
    size_t sizes[MAX_WORLD_COUNT];
    uint8_t packets[MAX_WORLD_COUNT][SERVER_LIST_PACKET_MAX_LENGTH];
    size_t endSize;
    uint8_t endPacket[SERVER_LIST_END_PACKET_LENGTH];
};

struct ServerListResult handle_server_list(struct Client *client);

struct ServerStatusResult {
    int status;
    size_t size;
    uint8_t packet[SERVER_STATUS_PACKET_LENGTH];
};

struct ServerStatusResult handle_server_status(struct Client *client, uint8_t world);

struct CharacterListHandler;

struct CharacterListResult {
    int status;
    size_t size;
    uint8_t packet[CHARACTER_LIST_PACKET_MAX_LENGTH];
};

struct CharacterListHandler *character_list_handler_create(struct Client *client, uint8_t world, uint8_t channel);
struct CharacterListResult character_list_handler_handle(struct CharacterListHandler *handler, int status);
void character_list_handler_destroy(struct CharacterListHandler *handler);

struct NameCheckHandler;

struct NameCheckResult {
    int status;
    size_t size;
    uint8_t packet[NAME_CHECK_RESPONSE_PACKET_MAX_LENGTH];
};

struct NameCheckHandler *name_check_handler_create(struct Client *client, uint8_t name_len, char *name);
struct NameCheckResult name_check_handler_handle(struct NameCheckHandler *handler, int status);
void name_check_handler_destroy(struct NameCheckHandler *handler);

struct CreateCharacterHandler;

struct CreateCharacterResult {
    int status;
    size_t size;
    uint8_t packet[CREATE_CHARACTER_RESPONSE_PACKET_LENGTH];
};

struct CreateCharacterHandler *create_character_handler_create(struct Client *client, uint8_t name_len, char *name, uint16_t job, uint8_t gender, uint8_t skin, uint32_t hair, uint32_t face, uint32_t top, uint32_t bottom, uint32_t shoes, uint32_t weapon);
struct CreateCharacterResult create_character_handler_handle(struct CreateCharacterHandler *handler, int status);
void create_character_handler_destroy(struct CreateCharacterHandler *handler);

struct RegisterPicHandler;

struct RegisterPicResult {
    int status;
    int fd;
    size_t size;
    uint8_t packet[CHANNEL_IP_PACKET_LENGTH];
};

struct RegisterPicHandler *register_pic_handler_create(struct Client *client, uint32_t chr_id, uint8_t pic_len, char *pic);
struct RegisterPicResult register_pic_handler_handle(struct RegisterPicHandler *handler, int status);
void register_pic_handler_destroy(struct RegisterPicHandler *handler);

struct VerifyPicHandler;

struct VerifyPicResult {
    int status;
    int fd;
    size_t size;
    uint8_t packet[CHANNEL_IP_PACKET_LENGTH];
};

struct VerifyPicHandler *verify_pic_handler_create(struct Client *client, uint32_t chr_id, uint8_t pic_len, char *pic);
struct VerifyPicResult verify_pic_handler_handle(struct VerifyPicHandler *handler, int status);
void verify_pic_handler_destroy(struct VerifyPicHandler *handler);

#endif


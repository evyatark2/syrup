#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <argon2.h>

#include <evutil.h>

#include "../wz.h"
#include "../account.h"
#include "../database.h"
#include "../packet.h"
#include "handlers.h"
#include "config.h"

static uint8_t gender_to_u8(enum AccountGender gender);

struct LoginHandler {
    struct Client *client;
    struct DatabaseRequest *request;
    uint8_t nameLength;
    char name[ACCOUNT_NAME_MAX_LENGTH];
    uint8_t passLength;
    char pass[ACCOUNT_PASSWORD_MAX_LENGTH];
    uint8_t hwid[ACCOUNT_HWID_LENGTH];
    int state;
};

struct LoginHandler *login_handler_create(struct Client *client, uint8_t name_len, char *name, uint8_t pass_len, char *pass, uint8_t *hwid)
{
    struct LoginHandler *handler = malloc(sizeof(struct LoginHandler));
    if (handler == NULL)
        return NULL;


    handler->client = client;
    handler->nameLength = name_len;
    memcpy(handler->name, name, name_len);
    handler->passLength = pass_len;
    memcpy(handler->pass, pass, pass_len);
    memcpy(handler->hwid, hwid, ACCOUNT_HWID_LENGTH);

    handler->state = 0;

    return handler;
}

struct LoginHandlerResult login_handler_handle(struct LoginHandler *handler, int status)
{
    while (true) {
        if (handler->state == 0) {
            struct RequestParams params = {
                .type = DATABASE_REQUEST_TYPE_TRY_CREATE_ACCOUNT,
                .tryCreateAccount = {
                    .nameLength = handler->nameLength,
                }
            };
            params.tryCreateAccount.salt = random();
            params.tryCreateAccount.salt <<= 32;
            params.tryCreateAccount.salt |= random();
            // TODO: Calibrate parameters
            argon2id_hash_raw(1, 128, 4, handler->pass, handler->passLength, &params.tryCreateAccount.salt, 8, params.tryCreateAccount.hash, ACCOUNT_HASH_LEN);

            memcpy(params.tryCreateAccount.name, handler->name, handler->nameLength);
            handler->request = database_request_create(handler->client->conn, &params);
            handler->state++;
            status = database_request_execute(handler->request, 0);
            if (status != 0)
                return (struct LoginHandlerResult) { status };
            handler->state++;
        }

        if (handler->state == 1) {
            status = database_request_execute(handler->request, status);
            if (status != 0)
                return (struct LoginHandlerResult) { status };
            handler->state++;
        }

        if (handler->state == 2) {
            handler->state++;
            const union DatabaseResult *res = database_request_result(handler->request);
            if (res->tryCreateAccount.created) {
                handler->client->node = account_login(res->tryCreateAccount.id);
                if (handler->client->node == NULL)
                    ; // TODO
                handler->client->account = account_get_default_account(handler->nameLength, handler->name);
                struct LoginHandlerResult ret = { 0, LOGIN_FAILURE_PACKET_LENGTH };
                login_failure_packet(LOGIN_FAILURE_REASON_TOS, ret.packet);
                database_request_destroy(handler->request);
                return ret;
            } else {
                database_request_destroy(handler->request);
                struct RequestParams params = {
                    .type = DATABASE_REQUEST_TYPE_GET_ACCOUNT_CREDENTIALS,
                    .getAccountCredentials = {
                        .nameLength = handler->nameLength
                    },
                };
                memcpy(params.getAccountCredentials.name, handler->name, handler->nameLength);
                handler->request = database_request_create(handler->client->conn, &params);
                status = database_request_execute(handler->request, 0);
                if (status != 0)
                    return (struct LoginHandlerResult) { status };
                handler->state++;
            }
        }

        if (handler->state == 3) {
            status = database_request_execute(handler->request, status);
            if (status != 0)
                return (struct LoginHandlerResult) { status };
            handler->state++;
        }

        if (handler->state == 4) {
            const union DatabaseResult *res = database_request_result(handler->request);
            if (!res->getAccountCredentials.found) {
                // A very rare case where the account got deleted while trying to get its credentials, try creating it again
                database_request_destroy(handler->request);
                handler->state = 0;
            } else {
                uint8_t hash[ACCOUNT_HASH_LEN];
                argon2id_hash_raw(1, 128, 4, handler->pass, handler->passLength, &res->getAccountCredentials.salt, 8, hash, ACCOUNT_HASH_LEN);
                if (memcmp(res->getAccountCredentials.hash, hash, ACCOUNT_HASH_LEN) != 0) {
                    struct LoginHandlerResult ret = { 0, LOGIN_FAILURE_PACKET_LENGTH };
                    login_failure_packet(LOGIN_FAILURE_REASON_INCORRECT_PASSWORD, ret.packet);
                    database_request_destroy(handler->request);
                    return ret;
                }

                handler->client->node = account_login(res->getAccountCredentials.id);
                if (handler->client->node == NULL) {
                    struct LoginHandlerResult ret = { 0, LOGIN_FAILURE_PACKET_LENGTH };
                    login_failure_packet(LOGIN_FAILURE_REASON_LOGGED_IN, ret.packet);
                    database_request_destroy(handler->request);
                    return ret;
                }

                // Account logged in now
                // Start loading the rest of the account's data

                handler->client->account.nameLength = handler->nameLength;
                memcpy(handler->client->account.name, handler->name, handler->nameLength);

                const struct RequestParams params = {
                    .type = DATABASE_REQUEST_TYPE_GET_ACCOUNT,
                    .getAccount = {
                        .id = res->getAccountCredentials.id
                    },
                };

                database_request_destroy(handler->request);
                handler->request = database_request_create(handler->client->conn, &params);
                handler->state++;
                status = database_request_execute(handler->request, 0);
                if (status != 0)
                    return (struct LoginHandlerResult) { status };
                handler->state++;
            }

        }

        if (handler->state == 5) {
            status = database_request_execute(handler->request, status);
            if (status != 0)
                return (struct LoginHandlerResult) { status };
            handler->state++;
        }

        if (handler->state == 6) {
            const union DatabaseResult *res = database_request_result(handler->request);
            handler->client->account.picLength = res->getAccount.picLength;
            memcpy(handler->client->account.pic, res->getAccount.pic, res->getAccount.picLength);
            handler->client->account.tos = res->getAccount.tos == 1;
            handler->client->account.gender = res->getAccount.isGenderNull ? ACCOUNT_GENDER_UNSPECIFIED : (res->getAccount.gender == 0 ? ACCOUNT_GENDER_MALE : ACCOUNT_GENDER_FEMALE);
            struct LoginHandlerResult ret = { 0 };
            ret.size = login_success_packet(account_get_id(handler->client->node), gender_to_u8(handler->client->account.gender), handler->client->account.nameLength, handler->client->account.name, handler->client->account.picLength == 0 ? PIC_STATUS_REGISTER : PIC_STATUS_REQUEST, ret.packet);
            database_request_destroy(handler->request);
            return ret;
        }
    }

    return (struct LoginHandlerResult) { -1 };
}

void login_handler_destroy(struct LoginHandler *handler)
{
    free(handler);
}

struct LogoutHandler {
    struct Client *client;
    struct DatabaseRequest *request;
    int state;
};

struct LogoutHandler *logout_handler_create(struct Client *client)
{
    struct LogoutHandler *handler = malloc(sizeof(struct LogoutHandler));
    if (handler == NULL)
        return NULL;

    handler->client = client;
    handler->state = 0;

    return handler;
}

int logout_handler_handle(struct LogoutHandler *handler, int status)
{
    if (handler->state == 0) {
        struct RequestParams params = {
            .type = DATABASE_REQUEST_TYPE_UPDATE_ACCOUNT,
            .updateAccount = {
                .id = account_get_id(handler->client->node),
                .picLength = handler->client->account.picLength,
                .tos = handler->client->account.tos ? 1 : 0,
                .isGenderNull = handler->client->account.gender == ACCOUNT_GENDER_UNSPECIFIED,
                .gender = handler->client->account.gender == ACCOUNT_GENDER_MALE ? 0 : 1
            }
        };

        memcpy(params.updateAccount.pic, handler->client->account.pic, handler->client->account.picLength);

        handler->request = database_request_create(handler->client->conn, &params);
        handler->state++;
        status = database_request_execute(handler->request, 0);
        if (status < 0 && !handler->client->loggedIn)
            account_logout(&handler->client->node);

        if (status != 0)
            return status;
        handler->state++;
    }

    if (handler->state == 1) {
        status = database_request_execute(handler->request, 0);
        if (status < 0 && !handler->client->loggedIn)
            account_logout(&handler->client->node);
        if (status != 0)
            return status;
        handler->state++;
    }

    if (handler->state == 2) {
        database_request_destroy(handler->request);
        if (!handler->client->loggedIn)
            account_logout(&handler->client->node);
    }

    return 0;
}

void logout_handler_destroy(struct LogoutHandler *handler)
{
    free(handler);
}

struct TosHandlerResult handle_tos(struct Client *client, bool accept)
{
    client->account.tos = accept;
    if (!client->account.tos)
        return (struct TosHandlerResult) { -1 };

    struct TosHandlerResult res = { 0 };
    res.size = login_success_packet(account_get_id(client->node), gender_to_u8(client->account.gender), client->account.nameLength, client->account.name, client->account.picLength == 0 ? PIC_STATUS_REGISTER : PIC_STATUS_REQUEST, res.packet);
    return res;
}

struct GenderHandler {
    struct Client *client;
    struct DatabaseRequest *request;
    enum AccountGender gender;
    int state;
};

struct GenderHandlerResult gender_handler_create(struct Client *client, enum AccountGender gender, struct GenderHandler **handler)
{
    if (gender != ACCOUNT_GENDER_UNSPECIFIED) {
        client->account.gender = gender;

        struct GenderHandlerResult res = { 0 };
        res.size = login_success_packet(account_get_id(client->node), gender_to_u8(client->account.gender), client->account.nameLength, client->account.name, client->account.picLength == 0 ? PIC_STATUS_REGISTER : PIC_STATUS_REQUEST, res.packet);
        return res;
    }

    *handler = malloc(sizeof(struct GenderHandler));
    if (*handler == NULL)
        return (struct GenderHandlerResult) { -1 };

    (*handler)->client = client;
    (*handler)->gender = gender;
    (*handler)->state = 0;

    return (struct GenderHandlerResult) { 1 };
}

struct GenderHandlerResult gender_handler_handle(struct GenderHandler *handler, int status)
{
    if (handler->state == 0) {
        struct RequestParams params = {
            .type = DATABASE_REQUEST_TYPE_UPDATE_ACCOUNT,
            .updateAccount = {
                .id = account_get_id(handler->client->node),
                .picLength = handler->client->account.picLength,
                .tos = handler->client->account.tos,
                .isGenderNull = handler->client->account.gender == ACCOUNT_GENDER_UNSPECIFIED,
                .gender = handler->client->account.gender,
            },
        };

        memcpy(params.updateAccount.pic, handler->client->account.pic, handler->client->account.picLength);

        handler->request = database_request_create(handler->client->conn, &params);
        if (handler->request == NULL)
            return (struct GenderHandlerResult) { -1 };
        handler->state++;
        status = database_request_execute(handler->request, 0);
        if (status != 0)
            return (struct GenderHandlerResult) { status };
        handler->state++;
    }

    if (handler->state == 1) {
        status = database_request_execute(handler->request, status);
        if (status != 0)
            return (struct GenderHandlerResult) { status };
        handler->state++;
    }

    if (handler->state == 2) {
        database_request_destroy(handler->request);
    }

    struct GenderHandlerResult res = { 0 };
    return res;
}

void gender_handler_destroy(struct GenderHandler *handler)
{
    free(handler);
}

struct PinResult handle_pin(struct Client *client, uint8_t c2, uint8_t c3)
{
    if (c2 == 1 && c3 == 0) {
        struct PinResult res = { 0, PIN_PACKET_LENGTH };
        pin_packet(PIN_PACKET_MODE_ACCEPT, res.packet);
        return res;
    }

    return (struct PinResult) { -1 };
}

struct ServerListResult handle_server_list(struct Client *client)
{
    struct ServerListResult res = { 0, .endSize = SERVER_LIST_END_PACKET_LENGTH };
    res.worldCount = 2;
    res.sizes[0] = server_list_packet(0, WORLD_FLAG_HOT, strlen("Hello!"), "Hello!", res.packets[0]);
    res.sizes[1] = server_list_packet(1, WORLD_FLAG_NEW, strlen("Howdy!"), "Howdy!", res.packets[1]);
    server_list_end_packet(res.endPacket);
    return res;
}

struct ServerStatusResult handle_server_status(struct Client *client, uint8_t world)
{
    struct ServerStatusResult res = { 0, SERVER_STATUS_PACKET_LENGTH };
    server_status_packet(world == 0 ? SERVER_STATUS_NORMAL : SERVER_STATUS_CRITICAL, res.packet);
    return res;
}

struct CharacterListHandler {
    struct Client *client;
    struct DatabaseRequest *req;
    int state;
};

struct CharacterListHandler *character_list_handler_create(struct Client *client, uint8_t world, uint8_t channel)
{
    struct CharacterListHandler *handler = malloc(sizeof(struct CharacterListHandler));
    if (handler == NULL)
        return NULL;

    client->world = world;
    client->channel = channel;

    handler->client = client;
    handler->state = 0;

    return handler;
}

struct CharacterListResult character_list_handler_handle(struct CharacterListHandler *handler, int status)
{
    if (handler->state == 0) {
        struct RequestParams params = {
            .type = DATABASE_REQUEST_TYPE_GET_CHARACTERS_FOR_ACCOUNT_FOR_WORLD,
            .getCharactersForAccountForWorld = {
                .id = account_get_id(handler->client->node),
                .world = handler->client->world
            },
        };
        handler->req = database_request_create(handler->client->conn, &params);
        if (handler->req == NULL)
            return (struct CharacterListResult) { -1 };

        handler->state++;
        if ((status = database_request_execute(handler->req, status)) < 0)
            goto fail;
        else if (status > 0)
            return (struct CharacterListResult) { status };

        handler->state++;
    }

    if (handler->state == 1) {
        if ((status = database_request_execute(handler->req, status)) < 0)
            goto fail;
        else if (status > 0)
            return (struct CharacterListResult) { status };
        handler->state++;
    }

    if (handler->state == 2) {
        struct CharacterListResult ret = { 0 };
        const union DatabaseResult *res = database_request_result(handler->req);
        struct CharacterStats chars[ACCOUNT_MAX_CHARACTERS_PER_WORLD];
        for (uint8_t i = 0; i < res->getCharactersForAccountForWorld.characterCount; i++) {

            chars[i].id = res->getCharactersForAccountForWorld.characters[i].id;
            chars[i].info.appearance.nameLength = res->getCharactersForAccountForWorld.characters[i].nameLength;
            memcpy(chars[i].info.appearance.name, res->getCharactersForAccountForWorld.characters[i].name, res->getCharactersForAccountForWorld.characters[i].nameLength);
            chars[i].info.appearance.gender = res->getCharactersForAccountForWorld.characters[i].gender;
            chars[i].info.appearance.skin = res->getCharactersForAccountForWorld.characters[i].skin;
            chars[i].info.appearance.face = res->getCharactersForAccountForWorld.characters[i].face;
            chars[i].info.appearance.hair = res->getCharactersForAccountForWorld.characters[i].hair;

            chars[i].info.appearance.gachaExp = 0;
            chars[i].info.appearance.map = 0;
            chars[i].info.appearance.spawnPoint = 0;

            for (uint8_t j = 0; j < EQUIP_SLOT_COUNT; j++)
                chars[i].info.appearance.equipments[j].isEmpty = true;

            for (uint8_t j = 0; j < res->getCharactersForAccountForWorld.characters[i].equipCount; j++) {
                chars[i].info.appearance.equipments[equip_slot_to_compact(equip_slot_from_id(res->getCharactersForAccountForWorld.characters[i].equipment[j]))].isEmpty = false;
                chars[i].info.appearance.equipments[equip_slot_to_compact(equip_slot_from_id(res->getCharactersForAccountForWorld.characters[i].equipment[j]))].id =
                    res->getCharactersForAccountForWorld.characters[i].equipment[j];
            }

            chars[i].info.level = res->getCharactersForAccountForWorld.characters[i].level;
            chars[i].info.job = res->getCharactersForAccountForWorld.characters[i].job;
            chars[i].info.fame = res->getCharactersForAccountForWorld.characters[i].fame;

            chars[i].str = res->getCharactersForAccountForWorld.characters[i].str;
            chars[i].dex = res->getCharactersForAccountForWorld.characters[i].dex;
            chars[i].int_ = res->getCharactersForAccountForWorld.characters[i].int_;
            chars[i].luk = res->getCharactersForAccountForWorld.characters[i].luk;
            chars[i].hp = res->getCharactersForAccountForWorld.characters[i].maxHp;
            chars[i].maxHp = res->getCharactersForAccountForWorld.characters[i].hp;
            chars[i].mp = res->getCharactersForAccountForWorld.characters[i].maxMp;
            chars[i].maxMp = res->getCharactersForAccountForWorld.characters[i].mp;
            chars[i].ap = res->getCharactersForAccountForWorld.characters[i].ap;
            chars[i].sp = res->getCharactersForAccountForWorld.characters[i].sp;
            chars[i].exp = res->getCharactersForAccountForWorld.characters[i].exp;
        }

        ret.size = character_list_packet(0, res->getCharactersForAccountForWorld.characterCount, chars, handler->client->account.picLength == 0 ? PIC_STATUS_REGISTER : PIC_STATUS_REQUEST, ret.packet);
        database_request_destroy(handler->req);
        return ret;
    }

    assert(false);

fail:
    database_request_destroy(handler->req);
    return (struct CharacterListResult) { status };
}

void character_list_handler_destroy(struct CharacterListHandler *handler)
{
    free(handler);
}

struct NameCheckHandler {
    struct Client *client;
    struct DatabaseRequest *req;
    uint8_t nameLength;
    char name[CHARACTER_MAX_NAME_LENGTH];
    int state;
};

struct NameCheckHandler *name_check_handler_create(struct Client *client, uint8_t name_len, char *name)
{
    struct NameCheckHandler *handler = malloc(sizeof(struct NameCheckHandler));
    if (handler == NULL)
        return NULL;

    handler->client = client;
    handler->nameLength = name_len;
    memcpy(handler->name, name, name_len);
    handler->state = 0;

    return handler;
}

struct NameCheckResult name_check_handler_handle(struct NameCheckHandler *handler, int status)
{
    if (handler->state == 0) {
        struct RequestParams params = {
            .type = DATABASE_REQUEST_TYPE_GET_CHARACTER_EXISTS,
            .getCharacterExists = {
                .nameLength = handler->nameLength,
            },
        };
        memcpy(params.getCharacterExists.name, handler->name, handler->nameLength);
        handler->req = database_request_create(handler->client->conn, &params);
        if (handler->req == NULL)
            return (struct NameCheckResult) { -1 };

        handler->state++;
        if ((status = database_request_execute(handler->req, status)) < 0)
            goto fail;
        else if (status > 0)
            return (struct NameCheckResult) { status };
        handler->state++;
    }

    if (handler->state == 1) {
        if ((status = database_request_execute(handler->req, status)) < 0)
            goto fail;
        else if (status > 0)
            return (struct NameCheckResult) { status };
        handler->state++;
    }

    if (handler->state == 2) {
        const union DatabaseResult *res = database_request_result(handler->req);
        struct NameCheckResult ret = { 0 };
        ret.size = name_check_response_packet(handler->nameLength, handler->name, !res->getCharacterExists.exists, ret.packet);
        database_request_destroy(handler->req);
        return ret;
    }

    assert(0);
fail:
    database_request_destroy(handler->req);
    return (struct NameCheckResult) { -1 };
}

void name_check_handler_destroy(struct NameCheckHandler *handler)
{
    free(handler);
}

struct CreateCharacterHandler {
    struct Client *client;
    struct DatabaseRequest *req;
    uint8_t nameLength;
    char name[CHARACTER_MAX_NAME_LENGTH];
    uint16_t job;
    uint8_t gender;
    uint8_t skin;
    uint32_t hair;
    uint32_t face;
    uint32_t top;
    uint32_t bottom;
    uint32_t shoes;
    uint32_t weapon;
    int state;
};

struct CreateCharacterHandler *create_character_handler_create(struct Client *client, uint8_t name_len, char *name, uint16_t job, uint8_t gender, uint8_t skin, uint32_t hair, uint32_t face, uint32_t top, uint32_t bottom, uint32_t shoes, uint32_t weapon)
{
    struct CreateCharacterHandler *handler = malloc(sizeof(struct CreateCharacterHandler));
    if (handler == NULL)
        return NULL;

    handler->client = client;
    handler->nameLength = name_len;
    memcpy(handler->name, name, name_len);
    handler->job = job;
    handler->gender = gender;
    handler->skin = skin;
    handler->hair = hair;
    handler->face = face;
    handler->top = top;
    handler->bottom = bottom;
    handler->shoes = shoes;
    handler->weapon = weapon;
    handler->state = 0;

    return handler;
}

struct CreateCharacterResult create_character_handler_handle(struct CreateCharacterHandler *handler, int status)
{
    if (handler->state == 0) {
        struct RequestParams params = {
            .type = DATABASE_REQUEST_TYPE_TRY_CREATE_CHARACTER,
            .tryCreateCharacter = {
                .nameLength = handler->nameLength,
                .accountId = account_get_id(handler->client->node),
                .world = handler->client->world,
                .map = 10000,
                .job = handler->job,
                .gender = handler->gender,
                .skin = handler->skin,
                .hair = handler->hair,
                .face = handler->face,
                .top = equipment_from_info(wz_get_equip_info(handler->top)),
                .bottom = equipment_from_info(wz_get_equip_info(handler->bottom)),
                .shoes = equipment_from_info(wz_get_equip_info(handler->shoes)),
                .weapon = equipment_from_info(wz_get_equip_info(handler->weapon)),
            },
        };

        memcpy(params.getCharacterExists.name, handler->name, handler->nameLength);
        handler->req = database_request_create(handler->client->conn, &params);
        if (handler->req == NULL)
            return (struct CreateCharacterResult) { -1 };

        handler->state++;
        if ((status = database_request_execute(handler->req, status)) < 0)
            goto fail;
        else if (status > 0)
            return (struct CreateCharacterResult) { status };
        handler->state++;
    }

    if (handler->state == 1) {
        if ((status = database_request_execute(handler->req, status)) < 0)
            goto fail;
        else if (status > 0)
            return (struct CreateCharacterResult) { status };
        handler->state++;
    }

    if (handler->state == 2) {
        const union DatabaseResult *res = database_request_result(handler->req);
        struct CharacterStats chr = {
            .id = res->tryCreateCharacter.id,
            .info = {
                .appearance = {
                    .nameLength = handler->nameLength,
                    //char name[CHARACTER_MAX_NAME_LENGTH];
                    .gender = handler->gender == 0 ? ACCOUNT_GENDER_MALE : ACCOUNT_GENDER_FEMALE,
                    .skin = handler->skin,
                    .face = handler->face,
                    .hair = handler->hair,
                    .gachaExp = 0,
                    .map = 10000,
                    .spawnPoint = 0,
                    //uint32_t equipments[EQUIP_SLOT_COUNT];
                },
                .level = 1,
                .job = handler->job,
                .fame = 0,
            },
            .str = 12,
            .dex = 5,
            .int_ = 4,
            .luk = 4,
            .hp = 50,
            .maxHp = 50,
            .mp = 5,
            .maxMp = 5,
            .ap = 0,
            .sp = 0,
            .exp = 0,
        };
        memcpy(chr.info.appearance.name, handler->name, handler->nameLength);
        for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++)
            chr.info.appearance.equipments[i].isEmpty = true;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_TOP_OR_OVERALL)].isEmpty = false;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_TOP_OR_OVERALL)].id = handler->top;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_BOTTOM)].isEmpty = false;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_BOTTOM)].id = handler->bottom;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_SHOES)].isEmpty = false;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_SHOES)].id = handler->shoes;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].isEmpty = false;
        chr.info.appearance.equipments[equip_slot_to_compact(EQUIP_SLOT_WEAPON)].id = handler->weapon;
        struct CreateCharacterResult ret = { 0 };
        ret.size = create_character_response_packet(&chr, ret.packet);
        database_request_destroy(handler->req);
        return ret;
    }

    assert(0);
fail:
    database_request_destroy(handler->req);
    return (struct CreateCharacterResult) { -1 };
}

void create_character_handler_destroy(struct CreateCharacterHandler *handler)
{
    free(handler);
}

struct RegisterPicHandler {
    struct Client *client;
    uint32_t characterId;
    int state;
};

struct RegisterPicHandler *register_pic_handler_create(struct Client *client, uint32_t chr_id, uint8_t pic_len, char *pic)
{
    struct RegisterPicHandler *handler = malloc(sizeof(struct RegisterPicHandler));
    if (handler == NULL)
        return NULL;

    client->account.picLength = pic_len;
    memcpy(client->account.pic, pic, pic_len);

    handler->client = client;
    handler->characterId = chr_id;
    handler->state = 0;

    return handler;
}

struct RegisterPicResult register_pic_handler_handle(struct RegisterPicHandler *handler, int status)
{
    if (handler->state == 0) {
        struct RegisterPicResult ret = { .status = POLLIN };
        ret.fd = assign_channel(handler->characterId, handler->client->world, handler->client->channel);
        if (ret.fd != -1) {
            handler->state++;
            return ret;
        } else {
            struct RegisterPicResult ret = { .status = 0, .size = LOGIN_ERROR_PACKET_LENGTH };
            login_error_packet(10, ret.packet);
            return ret;
        }
    }

    if (handler->state == 1) {
        if (status == 1) {
            account_set_cid(handler->client->node, handler->characterId);
            struct RegisterPicResult ret = { .status = 0, .size = CHANNEL_IP_PACKET_LENGTH };
            channel_ip_packet(LOGIN_CONFIG.worlds[handler->client->world].channels[handler->client->channel].ip, LOGIN_CONFIG.worlds[handler->client->world].channels[handler->client->channel].port, handler->characterId, ret.packet);
            handler->client->loggedIn = true;
            return ret;
        } else {
            struct RegisterPicResult ret = { .status = 0, .size = LOGIN_ERROR_PACKET_LENGTH };
            login_error_packet(10, ret.packet);
            return ret;
        }
    }

    return (struct RegisterPicResult) { .status = -1 };
}

void register_pic_handler_destroy(struct RegisterPicHandler *handler)
{
    free(handler);
}

struct VerifyPicHandler {
    struct Client *client;
    uint32_t characterId;
    uint8_t picLength;
    char pic[ACCOUNT_PIC_MAX_LENGTH];
    int state;
};

struct VerifyPicHandler *verify_pic_handler_create(struct Client *client, uint32_t chr_id, uint8_t pic_len, char *pic)
{
    struct VerifyPicHandler *handler = malloc(sizeof(struct VerifyPicHandler));
    if (handler == NULL)
        return NULL;

    handler->client = client;
    handler->characterId = chr_id;
    strncpy(handler->pic, pic, pic_len);
    handler->picLength = pic_len;
    handler->state = 0;

    return handler;
}

struct VerifyPicResult verify_pic_handler_handle(struct VerifyPicHandler *handler, int status)
{
    if (handler->client->account.picLength != handler->picLength) {
        // TODO: Incorrect PIC packet
        return (struct VerifyPicResult) { .status = 0, .size = 0};
    }

    if (memcmp(handler->client->account.pic, handler->pic, handler->picLength)) {
        // TODO: Incorrect PIC packet
        return (struct VerifyPicResult) { .status = 0, .size = 0 };
    }

    if (handler->state == 0) {
        struct VerifyPicResult ret = { .status = POLLIN };
        ret.fd = assign_channel(handler->characterId, handler->client->world, handler->client->channel);
        if (ret.fd != -1) {
            handler->state++;
            return ret;
        } else {
            struct VerifyPicResult ret = { .status = 0, .size = LOGIN_ERROR_PACKET_LENGTH };
            login_error_packet(10, ret.packet);
            return ret;
        }
    }

    if (handler->state == 1) {
        if (status == 1) {
            account_set_cid(handler->client->node, handler->characterId);
            struct VerifyPicResult ret = { .status = 0, .size = CHANNEL_IP_PACKET_LENGTH };
            channel_ip_packet(LOGIN_CONFIG.worlds[handler->client->world].channels[handler->client->channel].ip, LOGIN_CONFIG.worlds[handler->client->world].channels[handler->client->channel].port, handler->characterId, ret.packet);
            handler->client->loggedIn = true;
            return ret;
        } else {
            struct VerifyPicResult ret = { .status = 0, .size = LOGIN_ERROR_PACKET_LENGTH };
            login_error_packet(10, ret.packet);
            return ret;
        }
    }

    return (struct VerifyPicResult) { .status = -1 };
}

void verify_pic_handler_destroy(struct VerifyPicHandler *handler)
{
    free(handler);
}

static uint8_t gender_to_u8(enum AccountGender gender)
{
    return gender == ACCOUNT_GENDER_UNSPECIFIED ? 10 : (gender == ACCOUNT_GENDER_MALE ? 0 : 1);
}


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "client.h"
#include "server.h"
#include "handlers.h"
#include "../reader.h"
#include "config.h"

#define ACCOUNT_MAX_NAME_LENGTH 12
#define ACCOUNT_MAX_PASSWORD_LENGTH 12
#define ACCOUNT_HWID_LENGTH 10

struct LoginServer *SERVER;
static void on_log(enum LogType type, const char *fmt, ...);

static void *create_context(void);
static void destroy_context(void *ctx);
static struct SessionContainer *on_client_create(void *thread_ctx);
static int on_client_connect(struct SessionContainer *container, struct sockaddr *addr);
static int on_client_packet(struct SessionContainer *session, size_t size, uint8_t *packet);
static int on_resume_client_packet(struct SessionContainer *session, int fd, int status);
static void on_client_disconnect(struct SessionContainer *session);
static int on_resume_client_disconnect(struct SessionContainer *session, int fd, int status);
static void on_client_destroy(struct SessionContainer *session);
static void on_client_leave(uint32_t token);

static int on_database_lock_ready(struct SessionContainer *session, int fd, int status);
static int on_database_lock_ready_disconnect(struct SessionContainer *session, int fd, int status);

static void on_sigint(int sig);

int main(void)
{
    if (login_config_load("login/config.json") == -1)
        return -1;

    if (wz_init_equipment() != 0) {
        login_config_unload();
        return -1;
    }
    SERVER = login_server_create(on_log, create_context, destroy_context, on_client_create, on_client_connect, on_client_packet, on_client_disconnect, on_client_destroy, on_client_leave);
    if (SERVER == NULL) {
        wz_terminate_equipment();
        login_config_unload();
        return -1;
    }

    signal(SIGINT, on_sigint);
    login_server_start(SERVER);
    login_server_destroy(SERVER);
    wz_terminate_equipment();
    login_config_unload();
}

static void on_log(enum LogType type, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static void *create_context(void)
{
    const char *ip;
    const char *socket;
    struct in_addr addr4;
    struct in6_addr addr6;
    if (inet_pton(AF_INET, LOGIN_CONFIG.database.host, &addr4) == 1 || inet_pton(AF_INET6, LOGIN_CONFIG.database.host, &addr6) == 1) {
        ip = LOGIN_CONFIG.database.host;
        socket = NULL;
    } else {
        ip = NULL;
        socket = LOGIN_CONFIG.database.host;
    }

    return database_connection_create(ip, LOGIN_CONFIG.database.user, LOGIN_CONFIG.database.password, LOGIN_CONFIG.database.db, LOGIN_CONFIG.database.port, socket);
}

static void destroy_context(void *ctx)
{
    database_connection_destroy(ctx);
}

static struct SessionContainer *on_client_create(void *thread_ctx)
{
    struct Client *client = malloc(sizeof(struct Client));
    if (client == NULL)
        return NULL;

    client->conn = thread_ctx;
    client->node = NULL;
    client->loggedIn = false;

    return &client->session;
}

static int on_client_connect(struct SessionContainer *container, struct sockaddr *addr)
{
    return 0;
}

#define READER_BEGIN(size, packet) { \
        struct Reader reader__; \
        reader_init(&reader__, (size), (packet));

#define SKIP(size) \
        reader_skip(&reader__, (size))

#define READ_OR_KICK(func, ...) \
        if (!func(&reader__, ##__VA_ARGS__)) \
            return -1;

#define READER_END() \
        if (!reader_end(&reader__)) \
            return -1; \
    }


#define DEFINE_LOCK_AND_WRITE(T, t, R, status, size, packet) \
    static int t##_lock_and_write(struct SessionContainer *session, R (*handle)(T *handler, int status), void (*destroy)(T *handler)) \
    { \
        struct Client *client = (void *)session; \
        int fd = database_connection_lock(client->conn); \
        if (fd <= -2) { \
            R res = handle(client->handler, 0); \
            if (res.status > 0) { \
                session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet); \
            } else { \
                database_connection_unlock(client->conn); \
                destroy(client->handler); \
                if (res.status < 0) \
                    return res.status; \
                session_write(session->session, res.size, res.packet); \
            } \
        } else if (fd == -1) { \
            return -1; \
        } else { \
            session_set_event(session->session, POLLIN, fd, on_database_lock_ready); \
        } \
        return 0; \
    }

DEFINE_LOCK_AND_WRITE(struct LoginHandler, login_handler, struct LoginHandlerResult, status, size, packet)
DEFINE_LOCK_AND_WRITE(struct CharacterListHandler, character_list, struct CharacterListResult, status, size, packet)
DEFINE_LOCK_AND_WRITE(struct GenderHandler, gender_handler, struct GenderHandlerResult, status, size, packet)
DEFINE_LOCK_AND_WRITE(struct NameCheckHandler, name_check, struct NameCheckResult, status, size, packet)
DEFINE_LOCK_AND_WRITE(struct CreateCharacterHandler, create_character, struct CreateCharacterResult, status, size, packet)

static int on_client_packet(struct SessionContainer *session, size_t size, uint8_t *packet)
{
    if (size < 2)
        return -1;

    struct Client *client = (void *)session;
    uint16_t opcode;
    memcpy(&opcode, packet, sizeof(uint16_t));

    printf("Got packet with opcode %hu\n", opcode);
    for (size_t i = 0; i < size; i++) {
        printf("%02X ", packet[i]);
    }
    printf("\n\n");

    packet += 2;
    size -= 2;
    switch (opcode) {
    case 0x0001: {
        uint16_t name_len = ACCOUNT_MAX_NAME_LENGTH;
        char name[ACCOUNT_MAX_NAME_LENGTH];
        uint16_t pass_len = ACCOUNT_MAX_PASSWORD_LENGTH;
        char pass[ACCOUNT_MAX_PASSWORD_LENGTH];
        uint8_t hwid[ACCOUNT_HWID_LENGTH];
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_sized_string, &name_len, name);
        READ_OR_KICK(reader_sized_string, &pass_len, pass);
        READ_OR_KICK(reader_array, ACCOUNT_HWID_LENGTH, hwid);
        SKIP(17);
        READER_END();
        client->type = PACKET_TYPE_LOGIN;
        client->handler = login_handler_create(client, name_len, name, pass_len, pass, hwid);
        return login_handler_lock_and_write(session, login_handler_handle, login_handler_destroy);
    }
    break;

    case 0x0004:
    case 0x000B: {
        struct ServerListResult res = handle_server_list(client);
        if (res.status != 0)
            return -1;
        for (uint8_t i = 0; i < res.worldCount; i++)
            session_write(session->session, res.sizes[i], res.packets[i]);
        session_write(session->session, res.endSize, res.endPacket);
    }
    break;

    case 0x0005: {
        uint8_t world;
        uint8_t channel;
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_KICK(reader_u8, &world);
        READ_OR_KICK(reader_u8, &channel);
        SKIP(4);
        READER_END();
        client->type = PACKET_TYPE_CHARACTER_LIST;
        client->handler = character_list_handler_create(client, world, channel);
        return character_list_lock_and_write(session, character_list_handler_handle, character_list_handler_destroy);
    }
    break;

    case 0x0006: {
        uint8_t world;
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_u8, &world);
        SKIP(1);
        READER_END();
        struct ServerStatusResult res = handle_server_status(client, world);
        session_write(session->session, res.size, res.packet);
    }
    break;

    case 0x0007: {
        uint8_t tos;
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_u8, &tos);
        READER_END();
        struct TosHandlerResult res = handle_tos(client, tos == 1);
        if (res.status != 0)
            return -1;
        session_write(session->session, res.size, res.packet);
    }
    break;

    case 0x0008: {
        uint8_t ok;
        uint8_t gender;
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_u8, &ok);
        if (ok == 1)
        READ_OR_KICK(reader_u8, &gender);
        READER_END();

        client->type = PACKET_TYPE_GENDER;
        struct GenderHandlerResult res = gender_handler_create(client, ok == 1 ? (gender == 0 ? ACCOUNT_GENDER_MALE : ACCOUNT_GENDER_FEMALE) : ACCOUNT_GENDER_UNSPECIFIED, &client->handler);
        if (res.status > 0) {
            return gender_handler_lock_and_write(session, gender_handler_handle, gender_handler_destroy);
        } else if (res.status < 0) {
            gender_handler_destroy(client->handler);
            return -1;
        } else {
            session_write(session->session, res.size, res.packet);
        }
    }
    break;

    case 0x0009: {
        uint8_t c2;
        uint8_t c3; // TODO: This can be larger than the packet size
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_u8, &c2);
        READ_OR_KICK(reader_u8, &c3);
        READER_END();
        packet++;

        struct PinResult res = handle_pin(client, c2, c3);
        if (res.status != 0)
            return -1;
        session_write(session->session, res.size, res.packet);
    }
    break;

    case 0x0015: {
        uint16_t name_len = CHARACTER_MAX_NAME_LENGTH;
        char name[CHARACTER_MAX_NAME_LENGTH];
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_sized_string, &name_len, name);
        READER_END();
        client->type = PACKET_TYPE_NAME_CHECK;
        client->handler = name_check_handler_create(client, name_len, name);
        name_check_lock_and_write(session, name_check_handler_handle, name_check_handler_destroy);
    }
    break;

    case 0x0016: {
        uint16_t name_len = CHARACTER_MAX_NAME_LENGTH;
        char name[CHARACTER_MAX_NAME_LENGTH];
        uint32_t job;
        uint32_t face;
        uint32_t hair;
        uint32_t hair_color;
        uint32_t skin;
        uint32_t top;
        uint32_t bottom;
        uint32_t shoes;
        uint32_t weapon;
        uint8_t gender;
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_sized_string, &name_len, name);
        READ_OR_KICK(reader_u32, &job);
        READ_OR_KICK(reader_u32, &face);
        READ_OR_KICK(reader_u32, &hair);
        READ_OR_KICK(reader_u32, &hair_color);
        READ_OR_KICK(reader_u32, &skin);
        READ_OR_KICK(reader_u32, &top);
        READ_OR_KICK(reader_u32, &bottom);
        READ_OR_KICK(reader_u32, &shoes);
        READ_OR_KICK(reader_u32, &weapon);
        READ_OR_KICK(reader_u8, &gender);
        READER_END();
        client->type = PACKET_TYPE_CREATE_CHARACTER;
        client->handler = create_character_handler_create(client, name_len, name, job == 0 ? JOB_NOBLESSE : (job == 1 ? JOB_BEGINNER : JOB_LEGEND), gender, skin, hair + hair_color, face, top, bottom, shoes, weapon);
        return create_character_lock_and_write(session, create_character_handler_handle, create_character_handler_destroy);
    }
    break;

    case 0x001D: {
        uint32_t id;
        uint16_t pic_len = ACCOUNT_PIC_MAX_LENGTH;
        char pic[ACCOUNT_PIC_MAX_LENGTH];
        uint16_t len;
        READER_BEGIN(size, packet);
        SKIP(1);
        READ_OR_KICK(reader_u32, &id);
        // MAC address
        READ_OR_KICK(reader_u16, &len);
        SKIP(len);

        // HWID
        READ_OR_KICK(reader_u16, &len);
        SKIP(len);

        READ_OR_KICK(reader_sized_string, &pic_len, pic);
        READER_END();

        client->type = PACKET_TYPE_REGISTER_PIC;
        client->handler = register_pic_handler_create(client, id, pic_len, pic);
        if (client->handler == NULL)
            return -1;

        struct RegisterPicResult res = register_pic_handler_handle(client->handler, 0);
        session_set_event(session->session, res.status, res.fd, on_resume_client_packet);
        return 0;
    }
    break;

    case 0x001E: {
        uint32_t id;
        uint16_t pic_len = ACCOUNT_PIC_MAX_LENGTH;
        char pic[ACCOUNT_PIC_MAX_LENGTH];
        uint16_t len;
        READER_BEGIN(size, packet);
        READ_OR_KICK(reader_sized_string, &pic_len, pic);
        READ_OR_KICK(reader_u32, &id);
        // MAC address
        READ_OR_KICK(reader_u16, &len);
        SKIP(len);

        // HWID
        READ_OR_KICK(reader_u16, &len);
        SKIP(len);
        READER_END();

        client->type = PACKET_TYPE_VERIFY_PIC;
        client->handler = verify_pic_handler_create(client, id, pic_len, pic);
        if (client->handler == NULL)
            return -1;

        struct VerifyPicResult res = verify_pic_handler_handle(client->handler, 0);
        if (res.status > 0)
            session_set_event(session->session, res.status, res.fd, on_resume_client_packet);
        else if (res.status < 0) {
            verify_pic_handler_destroy(client->handler);
            return -1;
        } else {
            session_write(session->session, res.size, res.packet);
            verify_pic_handler_destroy(client->handler);
        }
        return 0;
    }
    break;
    }

    return 0;
}

static int on_database_lock_ready(struct SessionContainer *session, int fd, int status)
{
    struct Client *client = (void *)session;
    close(fd); // This is assuming that event_free() doesn't touch the fd
    switch (client->type) {
    case PACKET_TYPE_LOGIN: {
        struct LoginHandlerResult res = login_handler_handle(client->handler, status);
        if (res.status > 0) {
            session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        login_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_GENDER: {
        struct GenderHandlerResult res = gender_handler_handle(client->handler, status);
        if (res.status > 0) {
            session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        gender_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_CHARACTER_LIST: {
        struct CharacterListResult res = character_list_handler_handle(client->handler, status);
        if (res.status > 0) {
            session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        character_list_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_NAME_CHECK: {
        struct NameCheckResult res = name_check_handler_handle(client->handler, status);
        if (res.status > 0) {
            session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        name_check_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_CREATE_CHARACTER: {
        struct CreateCharacterResult res = create_character_handler_handle(client->handler, status);
        if (res.status > 0) {
            session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        create_character_handler_destroy(client->handler);
        return res.status;
    }
    break;
    }

    return -1;
}

static int on_resume_client_packet(struct SessionContainer *session, int fd, int status)
{
    struct Client *client = (void *)session;
    switch (client->type) {
    case PACKET_TYPE_LOGIN: {
        struct LoginHandlerResult res = login_handler_handle(client->handler, status);
        if (res.status > 0) {
            if (res.status != session_get_event_disposition(session->session))
                session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        login_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_GENDER: {
        struct GenderHandlerResult res = gender_handler_handle(client->handler, status);
        if (res.status > 0) {
            if (res.status != session_get_event_disposition(session->session))
                session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        login_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_CHARACTER_LIST: {
        struct CharacterListResult res = character_list_handler_handle(client->handler, status);
        if (res.status > 0) {
            if (res.status != session_get_event_disposition(session->session))
                session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        login_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_NAME_CHECK: {
        struct NameCheckResult res = name_check_handler_handle(client->handler, status);
        if (res.status > 0) {
            if (res.status != session_get_event_disposition(session->session))
                session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        login_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_CREATE_CHARACTER: {
        struct CreateCharacterResult res = create_character_handler_handle(client->handler, status);
        if (res.status > 0) {
            if (res.status != session_get_event_disposition(session->session))
                session_set_event(session->session, res.status, database_connection_get_fd(client->conn), on_resume_client_packet);
            return 1;
        }

        database_connection_unlock(client->conn);
        if (res.status == 0)
            session_write(session->session, res.size, res.packet);
        login_handler_destroy(client->handler);
        return res.status;
    }
    break;

    case PACKET_TYPE_REGISTER_PIC: {
        uint64_t status;
        read(fd, &status, 8);
        close(fd);
        struct RegisterPicResult res = register_pic_handler_handle(client->handler, status);
        // For now, assume that the result must be 0
        register_pic_handler_destroy(client->handler);
        session_write(client->session.session, res.size, res.packet);
        return 0;
    }
    break;

    case PACKET_TYPE_VERIFY_PIC: {
        uint64_t status;
        read(fd, &status, 8);
        close(fd);
        struct VerifyPicResult res = verify_pic_handler_handle(client->handler, status);
        // For now, assume that the result must be 0
        verify_pic_handler_destroy(client->handler);
        session_write(client->session.session, res.size, res.packet);
        return 0;
    }
    break;
    }
}

static void on_client_disconnect(struct SessionContainer *session)
{
    struct Client *client = (void *)session;
    if (client->node != NULL) {
        client->handler = logout_handler_create(client);
        int fd = database_connection_lock(client->conn);
        if (fd <= -2) {
            int status = logout_handler_handle(client->handler, 0);
            if (status > 0) {
                session_set_event(session->session, status, database_connection_get_fd(client->conn), on_resume_client_disconnect);
            } else {
                database_connection_unlock(client->conn);
            }
        } else if (fd != -1) {
            session_set_event(session->session, POLLIN, fd, on_database_lock_ready_disconnect);
        }
    }
}

static int on_database_lock_ready_disconnect(struct SessionContainer *session, int fd, int status)
{
    struct Client *client = (void *)session;
    close(fd);
    status = logout_handler_handle(client->handler, status);
    if (status > 0) {
        session_set_event(session->session, status, database_connection_get_fd(client->conn), on_resume_client_disconnect);
        return 1;
    }

    database_connection_unlock(client->conn);
    logout_handler_destroy(client->handler);
    return 0;
}

static int on_resume_client_disconnect(struct SessionContainer *session, int fd, int status)
{
    struct Client *client = (void *)session;
    status = logout_handler_handle(client->handler, status);
    if (status > 0) {
        if (status != session_get_event_disposition(session->session))
            session_set_event(session->session, status, database_connection_get_fd(client->conn), on_resume_client_disconnect);
        return 1;
    }

    database_connection_unlock(client->conn);
    logout_handler_destroy(client->handler);
    return 0;
}

static void on_client_destroy(struct SessionContainer *session)
{
    struct Client *client = (void *)session;
    free(client);
}

static void on_client_leave(uint32_t id)
{
    account_logout_by_cid(id);
}

static void on_sigint(int sig)
{
    login_server_stop(SERVER);
}


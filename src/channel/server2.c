#define _GNU_SOURCE // accept4
#include "server2.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <arpa/inet.h>
#include <linux/sockios.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>

#include <event.h>

#include "../crypt.h"
#include "../database.h"
#include "../hash-map.h"
#include "../opcodes.h"
#include "../packet.h"
#include "../reader.h"
#include "config.h"
#include "cq.h"
#include "drop.h"
#include "drops.h"
#include "events.h"
#include "room.h"
#include "scripting/script-manager.h"
#include "session.h"
#include "shop.h"
#include "thread-coordinator.h"
#include "thread-pool.h"
#include "user.h"

#define READER_BEGIN(size, packet) { \
        struct Reader reader__; \
        reader_init(&reader__, (size), (packet));

#define SKIP(size) \
        reader_skip(&reader__, (size))

#define READ_OR_ERROR(func, ...) \
        if (!func(&reader__, ##__VA_ARGS__)) { \
            session_shutdown(session); \
            return; \
        }

#define READER_AVAILABLE() \
        (reader__.size - reader__.pos)

#define READER_END() \
    }

struct ChannelServer {
    struct ThreadPool *pool;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    bool new;
    bool connected;
    struct event_base *base;
    struct event *acceptor;
    mtx_t loginLock;
    struct event *loginListener;
    struct HashSetU32 *disconnectingSessions;
    struct RoomThreadCoordinator *coordinator;
    struct EventManager *eventManager;
    enum ScriptValueType args[10];
    struct ScriptManager *questManager;
    struct ScriptManager *portalManager;
    struct ScriptManager *npcManager;
    struct ScriptManager *mapManager;
    struct ScriptManager *reactorManager;
};

#define BUF_LEN 65536
struct Actor {
    struct ChannelServer *server;
    struct Session *session;
    int fd;
    uint8_t buf[BUF_LEN];
    size_t len;
    struct Event *read;
    struct User *user;
    struct Room *room;
    struct RoomMember *member;
    struct Player *player;
    struct HashSetU32 *questItems;

    struct DecryptionContext *dec;

    int efd;
    struct DatabaseRequest *req;

    uint32_t targetMap;
    uint8_t targetPortal;
};

struct ThreadContext {
    struct DatabaseConnection *conn;
    struct HashSetU32 *rooms;
};

struct InitSessionInfo {
    struct ChannelServer *server;
    int fd;
    struct Session *session;
};

struct WriteQueue {
    struct iovec *queue;
    size_t capacity;
    size_t start;
    size_t len;
};

struct Room;

struct IdRoom {
    uint32_t id;
    struct Room *room;
};

static void on_command(int, void *ctx);
static void on_login_server_connected(int, short, void *);
static void on_login_server_read(int, short, void *ctx);
static void on_session_connected(int, short, void *);
static void on_session_command(int, void *ctx);

static void wait_for_event(int sec, void (*f)(void *), void *ctx, void *user_data);

struct ChannelServer *channel_server_create(uint16_t port, const char *host)
{
    struct ChannelServer *server = malloc(sizeof(struct ChannelServer));
    if (server == NULL)
        return NULL;

    const char *ip;
    const char *unix;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    if (inet_pton(AF_INET, CHANNEL_CONFIG.database.host, &addr4) == 1 || inet_pton(AF_INET6, CHANNEL_CONFIG.database.host, &addr6) == 1) {
        ip = CHANNEL_CONFIG.database.host;
        unix = NULL;
    } else {
        ip = NULL;
        unix = CHANNEL_CONFIG.database.host;
    }

    // TODO: I don't think this initialization belongs here
    struct DatabaseConnection *conn = database_connection_create(ip, CHANNEL_CONFIG.database.user, CHANNEL_CONFIG.database.password, CHANNEL_CONFIG.database.db, CHANNEL_CONFIG.database.port, unix);
    drops_load_from_db(conn);
    shops_load_from_db(conn);
    database_connection_destroy(conn);

    server->coordinator = room_thread_coordinator_create();
    if (server->coordinator == NULL) {
        free(server);
        return NULL;
    }

    size_t nproc = sysconf(_SC_NPROCESSORS_ONLN);
    void *ctxs[nproc];
    for (size_t i = 0; i < nproc; i++) {
        ctxs[i] = malloc(sizeof(struct ThreadContext));
        if (ctxs[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                struct ThreadContext *ctx = ctxs[j];
                database_connection_destroy(ctx->conn);
                hash_set_u32_destroy(ctx->rooms);
                free(ctx);
            }

            room_thread_coordinator_destroy(server->coordinator);
            free(server);
            return NULL;

        }

        struct ThreadContext *ctx = ctxs[i];

        ctx->rooms = hash_set_u32_create(sizeof(struct IdRoom), offsetof(struct IdRoom, id));
        if (ctx->rooms == NULL) {
            for (size_t j = 0; j < i; j++) {
                struct ThreadContext *ctx = ctxs[j];
                database_connection_destroy(ctx->conn);
                hash_set_u32_destroy(ctx->rooms);
                free(ctx);
            }

            free(ctx);
            room_thread_coordinator_destroy(server->coordinator);
            free(server);
            return NULL;
        }

        ctx->conn = database_connection_create(ip, CHANNEL_CONFIG.database.user, CHANNEL_CONFIG.database.password, CHANNEL_CONFIG.database.db, CHANNEL_CONFIG.database.port, unix);
        if (ctx->conn == NULL) {
            for (size_t j = 0; j < i; j++) {
                struct ThreadContext *ctx = ctxs[j];
                database_connection_destroy(ctx->conn);
                hash_set_u32_destroy(ctx->rooms);
                free(ctx);
            }

            hash_set_u32_destroy(ctx->rooms);
            free(ctx);
            room_thread_coordinator_destroy(server->coordinator);
            free(server);
            return NULL;
        }
    }

    server->eventManager = event_manager_create(EVENT_COUNT);
    if (server->eventManager == NULL) {
        for (size_t i = 0; i < nproc; i++) {
            struct ThreadContext *ctx = ctxs[i];
            database_connection_destroy(ctx->conn);
            hash_set_u32_destroy(ctx->rooms);
            free(ctx);
        }

        room_thread_coordinator_destroy(server->coordinator);
        free(server);
        return NULL;
    }

    server->args[0] = SCRIPT_VALUE_TYPE_USERDATA;
    struct ScriptEntryPoint entries[2] = {
        {
            .name = "enter",
            .argCount = 1,
            .args = &server->args[0]
        }
    };

    server->portalManager = script_manager_create("script/portal", "def.lua", 1, entries);

    entries[0].name = "talk";
    server->args[1] = SCRIPT_VALUE_TYPE_USERDATA;
    entries[0].args = &server->args[1];
    server->npcManager = script_manager_create("script/npc", "def.lua", 1, entries);

    entries[0].name = "start";
    server->args[2] = SCRIPT_VALUE_TYPE_USERDATA;
    entries[0].args = &server->args[2];
    entries[1].name = "end_";
    entries[1].argCount = 1;
    server->args[3] = SCRIPT_VALUE_TYPE_USERDATA;
    entries[1].args = &server->args[3];
    server->questManager = script_manager_create("script/quest", "def.lua", 2, entries);

    server->pool = thread_pool_create(nproc, ctxs);
    if (server->pool == NULL) {
        event_manager_destroy(server->eventManager);
        for (size_t i = 0; i < nproc; i++) {
            struct ThreadContext *ctx = ctxs[i];
            database_connection_destroy(ctx->conn);
            hash_set_u32_destroy(ctx->rooms);
            free(ctx);
        }

        room_thread_coordinator_destroy(server->coordinator);
        free(server);
        return NULL;
    }

    server->base = event_base_new();
    if (server->base == NULL) {
    }

    int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        free(server);
        return NULL;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int) { 1 }, sizeof(int));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(fd, (void *)&addr, sizeof(struct sockaddr_in)) == -1) {
        close(fd);
        free(server);
        return NULL;
    }

    if (listen(fd, 0) == -1) {
        close(fd);
        free(server);
        return NULL;
    }

    server->acceptor = event_new(server->base, fd, EV_READ | EV_PERSIST, on_session_connected, server);
    event_add(server->acceptor, NULL);
    server->new = true;
    server->connected = false;

    int fds[2];
    if (pipe(fds) == -1) {
        close(fd);
        free(server);
        return NULL;
    }

    int domain;
    if (inet_pton(AF_INET, host, &addr4) == 1) {
        domain = PF_INET;
        memcpy(&server->addr, &addr4, sizeof(struct sockaddr_in));
        server->addrlen = sizeof(struct sockaddr_in);
    } else if (inet_pton(AF_INET6, host, &addr6) == 1) {
        domain = PF_INET6;
        memcpy(&server->addr, &addr6, sizeof(struct sockaddr_in6));
        server->addrlen = sizeof(struct sockaddr_in6);
    } else {
        domain = PF_UNIX;
        struct sockaddr_un addr = {
            .sun_family = AF_UNIX,
        };
        strcpy(addr.sun_path, host);
        memcpy(&server->addr, &addr, sizeof(struct sockaddr_un));
        server->addrlen = sizeof(struct sockaddr_un);
    }

    fd = socket(domain, SOCK_STREAM, 0);
    if (fd == -1) {
        close(fds[1]);
        close(fds[0]);
        free(server);
        return NULL;
    }

    if (bind(fd, (void *)&server->addr, server->addrlen) == -1) {
        close(fd);
        close(fds[1]);
        close(fds[0]);
        free(server);
        return NULL;
    }

    if (listen(fd, 0) == -1) {
        close(fd);
        close(fds[1]);
        close(fds[0]);
        free(server);
        return NULL;
    }

    server->loginListener = event_new(server->base, fd, EV_READ, on_login_server_connected, server);
    event_add(server->loginListener, NULL);

#ifdef X
#   undef X
#endif

#define X(ev) event_##ev##_init(server->eventManager, wait_for_event, server);
    TRANSPORT_EVENTS()
#undef X

    event_area_boss_init(server->eventManager, wait_for_event, server);
    event_elevator_init(server->eventManager, wait_for_event, server);
    event_global_respawn_init(server->eventManager, wait_for_event, server);

    wz_init();

    return server;
}

struct Event {
    void (*f)(void *);
    void *userData;
    struct event *ev;
};

static void on_event_ready(int, short, void *);

static void wait_for_event(int sec, void (*f)(void *), void *ctx, void *user_data)
{
    struct ChannelServer *server = user_data;
    struct Event *ev = malloc(sizeof(struct Event));
    if (ev == NULL)
        return;

    ev->f = f;
    ev->userData = ctx;
    ev->ev = evtimer_new(server->base, on_event_ready, ev);
    struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
    evtimer_add(ev->ev, &tv);
}

static void on_event_ready(int, short, void *user_data)
{
    struct Event *ev = user_data;
    event_free(ev->ev);
    ev->f(ev->userData);
    free(ev);
}

void channel_server_destroy(struct ChannelServer *server)
{
    if (server != NULL) {
        /*room_thread_coordinator_destroy(server->coordinator);
        if (server->connected) {
            close(io_continuous_event_fd(server->loginRead));
            io_continuous_event_free(server->loginRead);
        } else {
            close(io_event_fd(server->loginConnect));
            io_event_free(server->loginConnect);
        }
        io_continuous_event_free(server->command);
        close(io_continuous_event_fd(server->acceptor));
        io_continuous_event_free(server->acceptor);
        io_worker_destroy(server->io);*/
    }
    free(server);
}

int channel_server_start(struct ChannelServer *server)
{
    return event_base_dispatch(server->base);
}

void channel_server_stop(struct ChannelServer *server)
{
    //close(server->commandSink);
}

static void on_command(int, void *user_data)
{
    /*struct ChannelServer *server = user_data;
    int size;
    ioctl(io_continuous_event_fd(server->command), FIONREAD, &size);

    if (size == 0) {
        if (recv(io_continuous_event_fd(server->command), NULL, 0, 0) == -1)
            ;
    }*/
}

static void notify_login_server(void *data, void *ctx);

static void on_login_server_connected(int fd, short, void *user_data)
{
    struct ChannelServer *server = user_data;
    int loginfd = accept(fd, NULL, NULL);
    if (loginfd == -1)
        ; // TODO
    close(fd);
    uint8_t new = server->new ? 0 : 1;
    write(loginfd, &new, 1);
    if (server->addr.ss_family == AF_UNIX)
        unlink(((struct sockaddr_un *)&server->addr)->sun_path);

    event_free(server->loginListener);
    server->loginListener = event_new(server->base, loginfd, EV_READ | EV_PERSIST, on_login_server_read, server);
    if (server->loginListener == NULL) {
    }

    event_add(server->loginListener, NULL);

    server->new = false;
    mtx_lock(&server->loginLock);
    server->connected = true;
    hash_set_u32_foreach(server->disconnectingSessions, notify_login_server, &loginfd);
    mtx_unlock(&server->loginLock);
}

static void notify_login_server(void *data, void *ctx)
{
    write(*(int *)ctx, data, sizeof(uint32_t));
}

static void on_login_server_read(int fd, short, void *ctx)
{
    struct ChannelServer *server = ctx;
    uint32_t token;
    ssize_t readed = read(fd, &token, 4);
    if (readed == 0) {
        close(fd);
        event_free(server->loginListener);

        int domain;
        switch (server->addr.ss_family) {
        case AF_INET:
            domain = PF_INET;
        break;
        case AF_INET6:
            domain = PF_INET6;
        break;
        case AF_UNIX:
            domain = PF_UNIX;
        break;
        }

        fd = socket(domain, SOCK_STREAM, 0);
        if (fd == -1) {
        }

        if (bind(fd, (void *)&server->addr, server->addrlen) == -1) {
        }

        if (listen(fd, 0) == -1) {
        }

        server->loginListener = event_new(server->base, fd, EV_READ, on_login_server_connected, server);
        if (server->loginListener == NULL) {
        }

        event_add(server->loginListener, NULL);

        server->connected = false;
    }

    uint8_t buf[5];
    buf[0] = 0;
    memcpy(buf + 1, &token, 4);
    write(fd, buf, 5);
}

static void on_pending_session_read(struct Worker *, ssize_t, void *);

struct InitSession {
    struct ChannelServer *server;
    int fd;
    struct EncryptionContext *enc;
    struct event *write;
    uint8_t packet[16];
    size_t len;
};

static void on_session_write(int, short, void *);
static void create_actor(struct ChannelServer *server, struct InitSession *session);

static void on_session_connected(int fd, short, void *user_data)
{
    struct ChannelServer *server = user_data;
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(struct sockaddr_storage);
    int new = accept4(fd, (void *)&addr, &addr_len, SOCK_NONBLOCK);
    if (new == -1)
        return;

    struct InitSession *init = malloc(sizeof(struct InitSession));
    if (init == NULL) {
        close(new);
        return;
    }

    uint8_t iv[] = { 0, 0, 0, 0 };
    init->enc = encryption_context_new(iv);
    if (init->enc == NULL) {
        free(init);
        close(new);
        return;
    }

    const uint8_t *send_iv = encryption_context_get_iv(init->enc);
    memcpy(init->packet, (uint8_t[]) {
        0x0E,
        0x00,
        83, // MAPLE_VERSION
        0x00,
        0x01,
        0x00,
        49,
        iv[0],
        iv[1],
        iv[2],
        iv[3],
        send_iv[0],
        send_iv[1],
        send_iv[2],
        send_iv[3],
        8
    }, 16);

    init->server = server;
    init->fd = new;
    init->len = 16;

    ssize_t len = write(init->fd, init->packet + (16 - init->len), init->len);
    if (len != 16) {
        if (len == -1 && errno != EAGAIN) {
            encryption_context_destroy(init->enc);
            free(init);
            close(new);
        } else {
            if (len != -1)
                init->len -= len;
            init->write = event_new(server->base, new, EV_WRITE, on_session_write, init);
            if (init->write == NULL || event_add(init->write, NULL) == -1) {
                if (init->write != NULL)
                    event_free(init->write);
                encryption_context_destroy(init->enc);
                free(init);
                close(new);
            }
        }
        return;
    }

    create_actor(server, init);
}

static void on_session_write(int fd, short, void *user_data)
{
    struct InitSession *init = user_data;
    ssize_t len = write(init->fd, init->packet + (16 - init->len), init->len);
    if (len != 16) {
        if (len == -1 && errno != EAGAIN) {
            event_free(init->write);
            encryption_context_destroy(init->enc);
            free(init);
            close(fd);
        } else {
            if (len != -1)
                init->len -= len;
            if (event_add(init->write, NULL) == -1) {
                if (init->write != NULL)
                    event_free(init->write);
                encryption_context_destroy(init->enc);
                free(init);
                close(fd);
            }
        }
        return;
    }

    event_free(init->write);
    struct Session *session = session_create(init->enc, fd);
    if (session == NULL) {
        encryption_context_destroy(init->enc);
        free(init);
        close(fd);
        return;
    }

    create_actor(init->server, init);
}

static void on_new_session_joined(struct Worker *, void *);

static void create_actor(struct ChannelServer *server, struct InitSession *init)
{
    struct Actor *actor = malloc(sizeof(struct Actor));
    if (actor == NULL) {
        encryption_context_destroy(init->enc);
        close(init->fd);
        free(init);
        return;
    }

    actor->dec = decryption_context_new((uint8_t[]) { 0, 0, 0, 0 });
    if (actor->dec == NULL) {
        free(actor);
        encryption_context_destroy(init->enc);
        close(init->fd);
        free(init);
        return;
    }

    actor->questItems = hash_set_u32_create(sizeof(uint32_t), 0);
    if (actor->questItems == NULL) {
        decryption_context_destroy(actor->dec);
        free(actor);
        encryption_context_destroy(init->enc);
        close(init->fd);
        free(init);
        return;
    }

    actor->session = session_create(init->enc, init->fd);
    if (actor->session == NULL) {
        hash_set_u32_destroy(actor->questItems);
        decryption_context_destroy(actor->dec);
        free(actor);
        encryption_context_destroy(init->enc);
        close(init->fd);
        free(init);
        return;
    }

    actor->fd = init->fd;
    free(init);

    actor->server = server;
    actor->member = NULL;
    actor->user = NULL;

    ssize_t thread = room_thread_coordinator_get_init(server->coordinator);
    if (thread == -1) {
        session_destroy(actor->session);
        hash_set_u32_destroy(actor->questItems);
        decryption_context_destroy(actor->dec);
        close(actor->fd);
        free(actor);
        return;
    }

    struct Worker *greeter = thread_pool_get_worker(server->pool, thread);
    if (worker_command(greeter, on_new_session_joined, actor) == -1) {
        session_destroy(actor->session);
        hash_set_u32_destroy(actor->questItems);
        decryption_context_destroy(actor->dec);
        close(actor->fd);
        free(actor);
        return;
    }
}

static void on_new_session_joined(struct Worker *worker, void *user_data)
{
    struct Actor *actor = user_data;
    actor->len = 0;
    worker_read(worker, actor->fd, actor->buf, BUF_LEN, false, on_pending_session_read, user_data);
}

struct HandleCommandContext {
    struct Session *session;
    struct SessionCommandContext *command;
    size_t len;
    void *buf;
};

static void handle_session_command(void *thread_ctx, void *ctx);

/*static void on_session_command(int fd, void *ctx)
{
    struct ChannelServer *server = ctx;

    eventfd_t count;
    // TODO: Check if eventfd can have spurious wake-ups
    assert(eventfd_read(fd, &count) == 0);
    struct SessionCommandContext cmds[count];
    cq_peek(server->cq, count, cmds);
    for (size_t i = 0; i < count; i++) {
        struct SessionContext *ctx = session_get_context(cmds[i].session);
        if (ctx->shuttingDown)
            continue;

        struct SessionCommandContext *cmd = malloc(sizeof(struct SessionCommandContext));
        if (cmd == NULL)
            ; // TODO

        *cmd = cmds[i];
        thread_pool_post(server->pool, ctx->thread, handle_session_command, cmd);
    }
    cq_dequeue(server->cq, count);
}*/

/*static void handle_session_command(void *, void *ctx_)
{
    struct SessionCommandContext *cmd_ = ctx_;
    struct Session *session = cmd_->session;
    struct SessionCommand cmd = cmd_->cmd;
    free(cmd_);
    struct SessionContext *ctx = session_get_context(session);

    switch (cmd.type) {
    case SESSION_COMMAND_WARP:
        break;
    case SESSION_COMMAND_ADD_VISIBLE_ITEM:
        break;
    case SESSION_COMMAND_EFFECT:
        // TODO: Check if ctx->room and ctx->member are always valid
        room_member_effect(ctx->room, ctx->member, cmd.effect.effect_id);
        break;
    }
}*/

struct HandlePacketContext {
    struct Session *session;
    size_t len;
    void *buf;
};

static void handle_init_packet(struct Worker *worker, struct Actor *ctx);

static void on_continue_pending_session_read(struct Worker *worker, ssize_t res, void *user_data);

static void on_pending_session_read(struct Worker *worker, ssize_t res, void *user_data)
{
    struct Actor *actor = user_data;
    struct Session *session = actor->session;

    if (res <= 0) {
        if (true) {
            actor->fd = -1;
        } else {
            hash_set_u32_destroy(actor->questItems);
            decryption_context_destroy(actor->dec);
            close(actor->fd);
            free(actor);
        }
        return;
    }

    actor->len += res;
    if (actor->len > 12) {
        session_shutdown(session);
        return;
    }

    if (actor->len < 4) {
        worker_read(worker, actor->fd, actor->buf + actor->len, BUF_LEN - actor->len, true, on_pending_session_read, actor);
        return;
    }

    uint32_t header;
    memcpy(&header, actor->buf, sizeof(uint32_t));
    uint16_t packet_len = (header >> 16) ^ header;
    if (packet_len != 8) {
        if (true) {
            actor->fd = -1;
        } else {
            hash_set_u32_destroy(actor->questItems);
            decryption_context_destroy(actor->dec);
            close(actor->fd);
            free(actor);
        }
        return;
    }

    if (actor->len < 12) {
        worker_read(worker, actor->fd, actor->buf + actor->len, BUF_LEN - actor->len, true, on_continue_pending_session_read, actor);
        return;
    }

    handle_init_packet(worker, actor);
}

static void on_continue_pending_session_read(struct Worker *worker, ssize_t res, void *user_data)
{
    struct Actor *actor = user_data;
    struct Session *session = actor->session;

    if (res <= 0) {
        if (true) {
            actor->fd = -1;
        } else {
            hash_set_u32_destroy(actor->questItems);
            decryption_context_destroy(actor->dec);
            close(actor->fd);
            free(actor);
        }
        return;
    }

    actor->len += res;
    if (actor->len > 12) {
        session_shutdown(session);
        return;
    }

    if (actor->len < 12) {
        worker_read(worker, actor->fd, actor->buf + actor->len, BUF_LEN - actor->len, true, on_continue_pending_session_read, actor);
        return;
    }

    handle_init_packet(worker, actor);
}

void handle_packet(struct Worker *worker, ssize_t len, void *user_data);

static void on_pending_session_illegal_read(struct Worker *worker, ssize_t len, void *);
static void on_database_unlocked(struct Worker *worker, int, void *user_data);
static void continue_fetch_character(struct Worker *worker, int status, void *);

static void handle_init_packet(struct Worker *worker, struct Actor *actor)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Session *session = actor->session;

    uint8_t packet[8];
    memcpy(packet, actor->buf + 4, 8);
    decryption_context_decrypt(actor->dec, 8, packet);

    uint16_t opcode;
    memcpy(&opcode, packet, sizeof(uint16_t));
    if (opcode != 0x14) {
        session_shutdown(session);
        return;
    }

    uint32_t id;
    memcpy(&id, packet + 2, sizeof(uint32_t));

    session_set_id(session, id);

    actor->len = 0;
    actor->read = worker_read(worker, actor->fd, actor->buf, BUF_LEN, false, on_pending_session_illegal_read, actor);

    actor->efd = database_connection_lock(thread_ctx->conn);
    if (actor->efd >= -1) {
        if (actor->efd == -1)
            session_shutdown(session);
        else
            worker_poll(worker, actor->efd, POLLIN, on_database_unlocked, actor);
        return;
    }

    // ctx->efd <= -2 means that we have the lock
    actor->efd = -1;

    struct RequestParams params = {
        .type = DATABASE_REQUEST_TYPE_GET_CHARACTER,
        .getCharacter.id = session_id(session)
    };

    actor->req = database_request_create(thread_ctx->conn, &params);
    if (actor->req == NULL) {
        database_connection_unlock(thread_ctx->conn);
        session_shutdown(session);
        return;
    }

    continue_fetch_character(worker, 0, actor);
}

static void on_pending_session_illegal_read(struct Worker *worker, ssize_t len, void *user_data)
{
    if (len == -ECANCELED)
        return;

    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Session *session = actor->session;
    if (actor->efd != -1) {
        close(actor->fd);
        actor->fd = -1;
        return;
    }

    if (actor->req != NULL) {
        database_request_destroy(actor->req);
        database_connection_unlock(thread_ctx->conn);
    }

    session_destroy(session);
    hash_set_u32_destroy(actor->questItems);
    decryption_context_destroy(actor->dec);
    free(actor);
    return;
}

static void on_database_unlocked(struct Worker *worker, int, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Session *session = actor->session;
}

static void on_read_canceled(struct Worker *worker, int status, void *user_data);

static void continue_fetch_character(struct Worker *worker, int status, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct DatabaseConnection *conn = thread_ctx->conn;
    struct Actor *actor = user_data;
    struct Session *session = actor->session;

    if (actor->fd == -1) {
        database_request_destroy(actor->req);
        database_connection_unlock(conn);
        session_destroy(session);
        hash_set_u32_destroy(actor->questItems);
        decryption_context_destroy(actor->dec);
        free(actor);
        return;
    }

    status = database_request_execute(actor->req, status);
    if (status != 0) {
        if (status > 0) {
            worker_poll(worker, database_connection_get_fd(conn), status, continue_fetch_character, actor);
        } else if (status < 0) {
            database_request_destroy(actor->req);
            database_connection_unlock(conn);
            actor->req = NULL;
            session_shutdown(session);
        }

        return;
    }

    actor->efd = -1;

    const union DatabaseResult *res = database_request_result(actor->req);

    // Initalize the character
    struct Character chr = { .id = session_id(session) };

    chr.nameLength = res->getCharacter.nameLength;
    memcpy(chr.name, res->getCharacter.name, res->getCharacter.nameLength);
    chr.map = res->getCharacter.map;
    // Will be updated in on_client_join()
    //chr->x = info->x;
    //chr->y = info->y;
    //chr->fh = 0;
    //chr->stance = 6;
    chr.spawnPoint = wz_get_portal_info_by_name(chr.map, "sp")->id;
    chr.level = res->getCharacter.level;
    chr.job = res->getCharacter.job;
    chr.exp = res->getCharacter.exp;
    chr.maxHp = res->getCharacter.maxHp;
    chr.eMaxHp = 0;
    chr.hp = res->getCharacter.hp;
    chr.maxMp = res->getCharacter.maxMp;
    chr.eMaxMp = 0;
    chr.mp = res->getCharacter.mp;
    chr.str = res->getCharacter.str;
    chr.estr = 0;
    chr.dex = res->getCharacter.dex;
    chr.edex = 0;
    chr.int_ = res->getCharacter.int_;
    chr.eint = 0;
    chr.luk = res->getCharacter.luk;
    chr.eluk = 0;
    chr.hpmp = res->getCharacter.hpmp;
    chr.ap = res->getCharacter.ap;
    chr.sp = res->getCharacter.sp;
    chr.fame = res->getCharacter.fame;
    chr.gender = res->getCharacter.gender == 0 ? ACCOUNT_GENDER_MALE : ACCOUNT_GENDER_FEMALE;
    chr.skin = res->getCharacter.skin;
    chr.face = res->getCharacter.face;
    chr.hair = res->getCharacter.hair;
    chr.mesos = res->getCharacter.mesos;
    chr.gachaExp = 0;
    chr.equipmentInventory.slotCount = res->getCharacter.equipSlots;
    chr.inventory[0].slotCount = res->getCharacter.useSlots;
    chr.inventory[1].slotCount = res->getCharacter.setupSlots;
    chr.inventory[2].slotCount = res->getCharacter.etcSlots;
    chr.inventory[3].slotCount = 252;

    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++)
        chr.equippedEquipment[i].isEmpty = true;

    for (uint8_t i = 0; i < res->getCharacter.equippedCount; i++) {
        const struct DatabaseCharacterEquipment *src = &res->getCharacter.equippedEquipment[i];
        struct Equipment *dst = &chr.equippedEquipment[equip_slot_to_compact(equip_slot_from_id(res->getCharacter.equippedEquipment[i].equip.item.itemId))].equip;
        chr.equippedEquipment[equip_slot_to_compact(equip_slot_from_id(res->getCharacter.equippedEquipment[i].equip.item.itemId))].isEmpty = false;
        dst->id = src->id;
        dst->equipId = src->equip.id;
        dst->item.id = src->equip.item.id;
        dst->item.itemId = src->equip.item.itemId;
        //equip->item.cashId;
        //equip->item.sn; // What is this?

        dst->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
                                   //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
        dst->item.flags = src->equip.item.flags;
        //dst->item.expiration = src->expiration;
        dst->item.expiration = -1;
        dst->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
                                      //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
        dst->level = src->equip.level;
        dst->slots = src->equip.slots;
        dst->str = src->equip.str;
        chr.estr += dst->str;
        dst->dex = src->equip.dex;
        chr.edex += dst->dex;
        dst->int_ = src->equip.int_;
        chr.eint += dst->int_;
        dst->luk = src->equip.luk;
        chr.eluk += dst->luk;
        dst->hp = src->equip.hp;
        chr.eMaxHp += dst->hp;
        dst->mp = src->equip.mp;
        chr.eMaxMp += dst->mp;
        dst->atk = src->equip.atk;
        dst->matk = src->equip.matk;
        dst->def = src->equip.def;
        dst->mdef = src->equip.mdef;
        dst->acc = src->equip.acc;
        dst->avoid = src->equip.avoid;
        dst->hands = 0; //equip->hands = res->getCharacter.equippedEquipment[i].hands;
        dst->speed = src->equip.speed;
        dst->jump = src->equip.jump;
        dst->cash = wz_get_equip_info(dst->item.itemId)->cash;
    }

    for (uint8_t j = 0; j < chr.equipmentInventory.slotCount; j++)
        chr.equipmentInventory.items[j].isEmpty = true;

    for (uint8_t i = 0; i < res->getCharacter.equipCount; i++) {
        const struct DatabaseCharacterEquipment *src = &res->getCharacter.equipmentInventory[i].equip;
        struct Equipment *dst = &chr.equipmentInventory.items[res->getCharacter.equipmentInventory[i].slot].equip;
        chr.equipmentInventory.items[res->getCharacter.equipmentInventory[i].slot].isEmpty = false;
        dst->id = src->id;
        dst->equipId = src->equip.id;
        dst->item.id = src->equip.item.id;
        dst->item.itemId = src->equip.item.itemId;
        //equip->item.cashId;
        //equip->item.sn; // What is this?

        dst->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
                                   //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
        dst->item.flags = src->equip.item.flags;
        dst->item.expiration = -1; //equip->item.expiration = res->getCharacter.equippedEquipment[i].expiration;
        dst->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
                                      //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
        dst->level = src->equip.level;
        dst->slots = src->equip.slots;
        dst->str = src->equip.str;
        dst->dex = src->equip.dex;
        dst->int_ = src->equip.int_;
        dst->luk = src->equip.luk;
        dst->hp = src->equip.hp;
        dst->mp = src->equip.mp;
        dst->atk = src->equip.atk;
        dst->matk = src->equip.matk;
        dst->def = src->equip.def;
        dst->mdef = src->equip.mdef;
        dst->acc = src->equip.acc;
        dst->avoid = src->equip.avoid;
        //dst->hands = src->equip.hands;
        dst->hands = 0;
        dst->speed = src->equip.speed;
        dst->jump = src->equip.jump;
        dst->cash = wz_get_equip_info(dst->item.itemId)->cash;
    }

    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t j = 0; j < chr.inventory[i].slotCount; j++)
            chr.inventory[i].items[j].isEmpty = true;
    }

    for (uint16_t i = 0; i < res->getCharacter.itemCount; i++) {
        uint8_t inv = res->getCharacter.inventoryItems[i].item.itemId / 1000000 - 2;
        chr.inventory[inv].items[res->getCharacter.inventoryItems[i].slot].isEmpty = false;
        struct InventoryItem *item = &chr.inventory[inv].items[res->getCharacter.inventoryItems[i].slot].item;
        item->item.id = res->getCharacter.inventoryItems[i].item.id;
        item->item.itemId = res->getCharacter.inventoryItems[i].item.itemId;
        item->item.flags = res->getCharacter.inventoryItems[i].item.flags;
        item->item.ownerLength = 0;
        item->item.giftFromLength = 0;
        item->quantity = res->getCharacter.inventoryItems[i].count;
    }

    chr.storage.id = res->getCharacter.storage.id;
    chr.storage.slots = res->getCharacter.storage.slots;
    chr.storage.count = res->getCharacter.storageItemCount + res->getCharacter.storageEquipCount;
    chr.storage.mesos = res->getCharacter.storage.mesos;

    for (size_t i = 0; i < res->getCharacter.storageEquipCount; i++) {
        chr.storage.storage[res->getCharacter.storageEquipment[i].slot].isEquip = true;
        const struct DatabaseEquipment *src = &res->getCharacter.storageEquipment[i].equip;
        struct Equipment *dst = &chr.storage.storage[res->getCharacter.storageEquipment[i].slot].equip;
        dst->id = src->id;
        dst->equipId = 0;
        dst->item.id = src->item.id;
        dst->item.itemId = src->item.itemId;
        //equip->item.cashId;
        //equip->item.sn; // What is this?

        dst->item.ownerLength = 0; // equip->item.ownerLength = res->getCharacter.equippedEquipment[i].ownerLength;
                                   //memcpy(equip->item.owner, res->getCharacter.equippedEquipment[i].owner, res->getCharacter.equippedEquipment[i].ownerLength);
        dst->item.flags = src->item.flags;
        dst->item.expiration = -1; //equip->item.expiration = res->getCharacter.equippedEquipment[i].expiration;
        dst->item.giftFromLength = 0; //equip->item.giftFromLength = res->getCharacter.equippedEquipment[i].giverLength;
                                      //equip->item.giftFrom[CHARACTER_MAX_NAME_LENGTH];
        dst->level = src->level;
        dst->slots = src->slots;
        dst->str = src->str;
        dst->dex = src->dex;
        dst->int_ = src->int_;
        dst->luk = src->luk;
        dst->hp = src->hp;
        dst->mp = src->mp;
        dst->atk = src->atk;
        dst->matk = src->matk;
        dst->def = src->def;
        dst->mdef = src->mdef;
        dst->acc = src->acc;
        dst->avoid = src->avoid;
        //dst->hands = src->equip.hands;
        dst->hands = 0;
        dst->speed = src->speed;
        dst->jump = src->jump;
        dst->cash = wz_get_equip_info(dst->item.itemId)->cash;
    }

    for (size_t i = 0; i < res->getCharacter.storageItemCount; i++) {
        chr.storage.storage[res->getCharacter.storageItems[i].slot].isEquip = false;
        const struct DatabaseItem *src = &res->getCharacter.storageItems[i].item;
        struct InventoryItem *dst = &chr.storage.storage[res->getCharacter.storageItems[i].slot].item;
        dst->quantity = res->getCharacter.storageItems[i].count;
        dst->item.id = src->id;
        dst->item.itemId = src->itemId;
        dst->item.ownerLength = src->ownerLength;
        memcpy(dst->item.owner, src->owner, src->ownerLength);
        dst->item.flags = src->flags;
        dst->item.expiration = -1;
        dst->item.giftFromLength = 0;
    }

    chr.quests = hash_set_u16_create(sizeof(struct Quest), offsetof(struct Quest, id));
    if (chr.quests == NULL) {
        database_request_destroy(actor->req);
        database_connection_unlock(conn);
        session_shutdown(session);
        return;
    }

    for (size_t i = 0; i < res->getCharacter.questCount; i++) {
        struct Quest quest = {
            .id = res->getCharacter.quests[i],
            .progressCount = 0,
        };
        hash_set_u16_insert(chr.quests, &quest); // TODO: Check

        /*const struct QuestInfo *quest_info = wz_get_quest_info(res->getCharacter.quests[i]);
        for (size_t i = 0; i < quest_info->endRequirementCount; i++) {
            if (quest_info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_ITEM) {
                const struct ItemInfo *item_info = wz_get_item_info(quest_info->endRequirements[i].item.id);
                if (item_info->quest) {
                    //int32_t total = 0;
                    uint8_t inv = quest_info->endRequirements[i].item.id / 1000000;
                    if (inv == 1) {
                        for (uint8_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
                            if (!chr->equipmentInventory.items[j].isEmpty &&
                                    chr->equipmentInventory.items[j].equip.item.itemId == quest_info->endRequirements[i].item.id) {
                                //total++;
                            }
                        }
                    } else {
                        inv -= 2;
                        for (uint8_t j = 0; j < chr->inventory[inv].slotCount; j++) {
                            if (!chr->inventory[inv].items[j].isEmpty &&
                                    chr->inventory[inv].items[j].item.item.itemId == quest_info->endRequirements[i].item.id) {
                                //total += chr->inventory[inv].items[j].item.quantity;
                            }
                        }
                    }

                    //if (total < quest_info->endRequirements[i].item.count)
                    //    hash_set_u32_insert(chr->itemQuests, &quest_info->endRequirements[i].item.id);
                }
            }
        }*/
    }


    for (size_t i = 0; i < res->getCharacter.progressCount; i++) {
        struct Quest *quest = hash_set_u16_get(chr.quests, res->getCharacter.progresses[i].questId); // TODO: Check
        const struct QuestInfo *info = wz_get_quest_info(quest->id);
        uint8_t j;
        //int16_t amount;
        for (size_t i_ = 0; true; i_++) {
            struct QuestRequirement *req = &info->endRequirements[i_];
            if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
                for (size_t i_ = 0; true; i_++) {
                    if (req->mob.mobs[i_].id == res->getCharacter.progresses[i].progressId) {
                        j = i_;
                        //amount = req->mob.mobs[i_].count;
                        break;
                    }
                }
                break;
            }
        }

        /*if (res->getCharacter.progresses[i].progress < amount) {
            struct MonsterRefCount *m = hash_set_u32_get(chr->monsterQuests, res->getCharacter.progresses[i].progressId);
            if (m != NULL) {
                m->refs++;
            } else {
                struct MonsterRefCount new = {
                    .id = res->getCharacter.progresses[i].progressId,
                    .refs = 1
                };
                hash_set_u32_insert(chr->monsterQuests, &new);
            }
        }*/

        quest->progress[j] = res->getCharacter.progresses[i].progress;
        quest->progressCount++;
    }

    chr.questInfos = hash_set_u16_create(sizeof(struct QuestInfoProgress), offsetof(struct QuestInfoProgress, id));
    if (chr.questInfos == NULL) {
        hash_set_u16_destroy(chr.quests);
        database_request_destroy(actor->req);
        database_connection_unlock(conn);
        session_shutdown(session);
        return;
    }

    for (size_t i = 0; i < res->getCharacter.questInfoCount; i++) {
        struct QuestInfoProgress new = {
            .id = res->getCharacter.questInfos[i].infoId,
            .length = res->getCharacter.questInfos[i].progressLength,
        };

        memcpy(new.value, res->getCharacter.questInfos[i].progress, res->getCharacter.questInfos[i].progressLength);

        hash_set_u16_insert(chr.questInfos, &new);
    }

    chr.completedQuests = hash_set_u16_create(sizeof(struct CompletedQuest), offsetof(struct CompletedQuest, id));
    if (chr.completedQuests == NULL) {
        hash_set_u16_destroy(chr.questInfos);
        hash_set_u16_destroy(chr.quests);
        database_request_destroy(actor->req);
        database_connection_unlock(conn);
        session_shutdown(session);
        return;
    }

    for (size_t i = 0; i < res->getCharacter.completedQuestCount; i++) {
        struct tm tm = {
            .tm_sec = res->getCharacter.completedQuests[i].time.second,
            .tm_min = res->getCharacter.completedQuests[i].time.minute,
            .tm_hour = res->getCharacter.completedQuests[i].time.hour,
            .tm_mday = res->getCharacter.completedQuests[i].time.day,
            .tm_mon = res->getCharacter.completedQuests[i].time.month - 1,
            .tm_year = res->getCharacter.completedQuests[i].time.year - 1900,
            .tm_gmtoff = 0,
            .tm_isdst = 0
        };
        struct CompletedQuest quest = {
            .id = res->getCharacter.completedQuests[i].id,
            .time = timegm(&tm)
        };
        hash_set_u16_insert(chr.completedQuests, &quest); // TODO: Check
    }

    chr.skills = hash_set_u32_create(sizeof(struct Skill), offsetof(struct Skill, id));
    if (chr.skills == NULL) {
        hash_set_u16_destroy(chr.completedQuests);
        hash_set_u16_destroy(chr.quests);
        database_request_destroy(actor->req);
        database_connection_unlock(conn);
        actor->efd = -1;
        session_shutdown(session);
        return;
    }

    for (size_t i = 0; i < res->getCharacter.skillCount; i++) {
        struct Skill skill = {
            .id = res->getCharacter.skills[i].id,
            .level = res->getCharacter.skills[i].level,
            .masterLevel = res->getCharacter.skills[i].masterLevel,
        };
        hash_set_u32_insert(chr.skills, &skill); // TODO: Check
    }

    chr.monsterBook = hash_set_u32_create(sizeof(struct MonsterBookEntry), offsetof(struct MonsterBookEntry, id));
    if (chr.monsterBook == NULL) {
        hash_set_u32_destroy(chr.skills);
        hash_set_u16_destroy(chr.completedQuests);
        hash_set_u16_destroy(chr.questInfos);
        hash_set_u16_destroy(chr.quests);
        database_request_destroy(actor->req);
        database_connection_unlock(conn);
        session_shutdown(session);
        return;
    }

    for (size_t i = 0; i < res->getCharacter.monsterBookEntryCount; i++) {
        struct MonsterBookEntry entry = {
            .id = res->getCharacter.monsterBook[i].id,
            .count = res->getCharacter.monsterBook[i].quantity,
        };
        hash_set_u32_insert(chr.monsterBook, &entry);
    }

    memset(chr.keyMap, 0, KEYMAP_MAX_KEYS * sizeof(struct KeyMapEntry));
    for (size_t i = 0; i < res->getCharacter.keyMapEntryCount; i++) {
        chr.keyMap[res->getCharacter.keyMap[i].key].type = res->getCharacter.keyMap[i].type;
        chr.keyMap[res->getCharacter.keyMap[i].key].action = res->getCharacter.keyMap[i].action;
    }

    database_request_destroy(actor->req);
    database_connection_unlock(conn);
    actor->req = NULL;

    actor->player = malloc(sizeof(struct Player));
    if (actor->player == NULL) {
        hash_set_u32_destroy(chr.monsterBook);
        hash_set_u32_destroy(chr.skills);
        hash_set_u16_destroy(chr.completedQuests);
        hash_set_u16_destroy(chr.questInfos);
        hash_set_u16_destroy(chr.quests);
        session_shutdown(session);
        return;
    }

    actor->player->id = chr.id;
    actor->player->level = chr.level;
    actor->player->job = chr.job;
    actor->player->nameLength = chr.nameLength;
    strncpy(actor->player->name, chr.name, chr.nameLength);
    actor->player->appearance = character_to_character_appearance(&chr);
    actor->player->chair = 0;
    actor->player->x = 0;
    actor->player->y = 0;
    actor->player->stance = 0;
    actor->player->fh = 0;

    actor->user = user_create(session, &chr, actor->server->questManager, actor->server->portalManager, actor->server->npcManager, actor->server->mapManager);
    if (actor->user == NULL) {
        hash_set_u32_destroy(chr.monsterBook);
        hash_set_u32_destroy(chr.skills);
        hash_set_u16_destroy(chr.completedQuests);
        hash_set_u16_destroy(chr.questInfos);
        hash_set_u16_destroy(chr.quests);
        session_shutdown(session);
        return;
    }

    actor->targetMap = chr.map;
    actor->targetPortal = 0;

    worker_cancel(worker, actor->read, on_read_canceled, actor);
}

static void flush_character(struct Worker *worker, int status, void *);

static void on_writes_flushed(struct Session *session, void *);

void handle_packet(struct Worker *worker, ssize_t len, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Session *session = actor->session;
    struct User *user = actor->user;
    struct Room *room = actor->room;
    struct RoomMember *member = actor->member;

    // We only get the size of the buffer a single time as
    // doing it as part of the loop can starve other sessions
    // if this session keeps sending data

    if (len <= 0) {
        room_leave(room, member);

        room_thread_coordinator_unref(actor->server->coordinator, room_id(room));

        if (!room_keep_alive(room)) {
            hash_set_u32_remove(thread_ctx->rooms, room_id(room));
            room_destroy(actor->server->eventManager, room);
        }

        flush_character(worker, 0, actor);
        return;
    }

    actor->len += len;
    if (actor->len < 4) {
        worker_read(worker, actor->fd, actor->buf + actor->len, BUF_LEN - actor->len, true, handle_packet, actor);
        return;
    }

    len = actor->len;
    uint8_t *packet = actor->buf;
    while (len >= 4) {
        uint32_t header;
        memcpy(&header, packet, sizeof(uint32_t));

        uint16_t packet_len = (header >> 16) ^ header;
        // 1. Each packet must at least include an opcode
        // 2. Currently we only support a packet with a maximum size of 65532
        if (packet_len < 2 || packet_len > 65532) {
            session_shutdown(session);
            return;
        }

        if (len < 4 + packet_len) {
            break;
        }

        decryption_context_decrypt(actor->dec, packet_len, packet + 4);

        printf("Reading:\n");
        for (size_t i = 0; i < packet_len; i++) {
            printf("%02X ", packet[i+4]);
        }
        puts("");

        uint16_t opcode;
        memcpy(&opcode, packet + 4, 2);

        packet += 6;
        packet_len -= 2;

        switch (opcode) {
        case RECEIVE_OPCODE_PORTAL: {
            if (member == NULL)
                break;

            uint32_t target;
            uint16_t len = PORTAL_INFO_NAME_MAX_LENGTH;
            char portal[PORTAL_INFO_NAME_MAX_LENGTH+1];
            READER_BEGIN(packet_len, packet);
            SKIP(1);
            READ_OR_ERROR(reader_u32, &target);
            READ_OR_ERROR(reader_sized_string, &len, portal);
            SKIP(1);
            SKIP(2); // wheel
            READER_END();

            if (user_portal(user, target, len, portal, &actor->targetMap, &actor->targetPortal) == -1)
                break;

            room_leave(room, member);
            actor->member = NULL;
            member = NULL;

            room_thread_coordinator_unref(actor->server->coordinator, room_id(room));

            if (!room_keep_alive(room)) {
                hash_set_u32_remove(thread_ctx->rooms, room_id(room));
                room_destroy(actor->server->eventManager, room);
            }

            actor->room = NULL;
            room = NULL;
            session_wait_for_writes(session, on_writes_flushed, actor);
        }
        break;

        case RECEIVE_OPCODE_MOVE: {
            if (member == NULL)
                break;

            size_t len = 1;
            uint8_t count;
            READER_BEGIN(packet_len, packet);
            SKIP(9);
            READ_OR_ERROR(reader_u8, &count);
            for (uint8_t i = 0; i < count; i++) {
                uint8_t command;
                READ_OR_ERROR(reader_u8, &command);
                len++;
                switch (command) {
                case 0: case 5: case 17: {
                    int16_t x;
                    int16_t y;
                    uint16_t fh;
                    uint8_t stance;
                    //SKIP(13);
                    READ_OR_ERROR(reader_i16, &x);
                    READ_OR_ERROR(reader_i16, &y);
                    SKIP(4); // x y wobble
                    READ_OR_ERROR(reader_u16, &fh);
                    READ_OR_ERROR(reader_u8, &stance);
                    SKIP(2); // duration
                    len += 13;
                    room_member_update_stance(member, stance);
                    room_member_update_coords(member, x, y, fh);
                }
                    break;

                case 1: case 2: case 6: case 12: case 13: case 16: case 18: case 19: case 20: case 22: {
                    uint8_t stance;
                    SKIP(4); // Relative movement
                    READ_OR_ERROR(reader_u8, &stance);
                    SKIP(2); // duration
                    len += 7;
                    room_member_update_stance(member, stance);
                }
                break;

                case 3: case 4: case 7: case 8: case 9: case 11: {
                    uint8_t stance;
                    SKIP(8); // Relative movement, with wobble
                    READ_OR_ERROR(reader_u8, &stance);
                    len += 9;
                    room_member_update_stance(member, stance);
                }
                break;

                case 10: // Change equip
                SKIP(1);
                len += 1;
                break;

                case 14:
                SKIP(9);
                len += 9;
                break;

                case 15: {
                    uint8_t stance;
                    SKIP(12); // x, y, and wobbles, fh, origin fh
                    READ_OR_ERROR(reader_u8, &stance);
                    SKIP(2); // duration
                    len += 15;
                    room_member_update_stance(member, stance);
                }
                break;

                //case 21:
                //    SKIP(3);
                //    len += 3;
                //break;
                default: {
                    session_shutdown(session);
                    break;
                }
                }
            }
            SKIP(18);
            READER_END();

            room_member_move(room, member, len, packet + 9);
        }
        break;

        case RECEIVE_OPCODE_SIT: {
            uint16_t id;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u16, &id);
            READER_END();

            room_member_sit_packet(room, member, id);
        }
        break;

        case RECEIVE_OPCODE_CHAIR: {
            uint32_t id;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u32, &id);
            READER_END();

            if (!user_chair(user, id))
                break;

            room_member_chair(room, member, id);
            //session_enable_actions(session);
        }
        break;

        case RECEIVE_OPCODE_ATTACK: {
            uint32_t oids[15];
            int32_t damage[15 * 15];
            uint8_t monster_count;
            uint8_t hit_count;
            uint32_t skill;
            uint8_t display;
            uint8_t direction;
            uint8_t stance;
            uint8_t speed;
            READER_BEGIN(packet_len, packet);
            SKIP(1);
            READ_OR_ERROR(reader_u8, &hit_count);
            monster_count = hit_count >> 4;
            hit_count &= 0xF;
            READ_OR_ERROR(reader_u32, &skill);
            SKIP(8);
            READ_OR_ERROR(reader_u8, &display);
            READ_OR_ERROR(reader_u8, &direction);
            READ_OR_ERROR(reader_u8, &stance);
            SKIP(1);
            READ_OR_ERROR(reader_u8, &speed);
            SKIP(4);
            for (int8_t i = 0; i < monster_count; i++) {
                READ_OR_ERROR(reader_u32, &oids[i]);
                SKIP(14);

                for (int8_t j = 0; j < hit_count; j++)
                    READ_OR_ERROR(reader_i32, &damage[i * hit_count + j]);

                SKIP(4);
            }
            SKIP(4);
            READER_END();

            int8_t level = 0;
            if (skill != 0) {
                if (!user_use_skill(user, skill, &level, NULL))
                    break;
            }

            uint32_t killed[15];
            size_t kill_count;
            if (!room_member_close_range_attack(room, member, monster_count, hit_count, skill, level, display, direction, stance, speed, oids, damage, &kill_count, killed))
                break;

            bool leveled;
            if (!user_kill_monsters(user, kill_count, killed, &leveled))
                break;

            if (leveled)
                room_member_level_up(room, member);
        }
        break;

        case RECEIVE_OPCODE_RANGED_ATTACK: {
            uint32_t oids[15];
            int32_t damage[15 * 15];
            uint8_t monster_count;
            uint8_t hit_count;
            uint32_t skill;
            uint8_t display;
            uint8_t direction;
            uint8_t stance;
            uint8_t speed;
            //uint8_t ranged_direction;
            READER_BEGIN(packet_len, packet);
            SKIP(1);
            READ_OR_ERROR(reader_u8, &hit_count);
            monster_count = hit_count >> 4;
            hit_count &= 0xF;
            READ_OR_ERROR(reader_u32, &skill);
            SKIP(8);
            READ_OR_ERROR(reader_u8, &display);
            READ_OR_ERROR(reader_u8, &direction);
            READ_OR_ERROR(reader_u8, &stance);
            SKIP(1);
            READ_OR_ERROR(reader_u8, &speed);
            SKIP(1);
            //READ_OR_ERROR(reader_u8, &ranged_direction);
            SKIP(8);
            for (int8_t i = 0; i < monster_count; i++) {
                READ_OR_ERROR(reader_u32, &oids[i]);
                SKIP(14);

                for (int8_t j = 0; j < hit_count; j++)
                    READ_OR_ERROR(reader_i32, &damage[i * hit_count + j]);


                SKIP(4);
            }
            SKIP(4);
            READER_END();

            int8_t skill_level = 0;
            uint32_t projectile;
            if (skill == 0) {
                if (!user_use_projectile(user, skill, &projectile))
                    break;
            } else {
                if (!user_use_skill(user, skill, &skill_level, &projectile))
                    break;
            }

            size_t kill_count;
            uint32_t killed[16];
            if (!room_member_ranged_attack(room, member, monster_count, hit_count, skill, skill_level, display, direction, stance, speed, oids, damage, projectile, &kill_count, killed))
                break;

            bool leveled;
            if (!user_kill_monsters(user, kill_count, killed, &leveled))
                break;

            if (leveled)
                room_member_level_up(room, member);
        }
        break;

        case RECEIVE_OPCODE_MAGIC_ATTACK: {
            uint32_t oids[15];
            int32_t damage[15 * 15];
            uint8_t monster_count;
            uint8_t hit_count;
            uint32_t skill;
            uint8_t display;
            uint8_t direction;
            uint8_t stance;
            uint8_t speed;
            READER_BEGIN(packet_len, packet);
            SKIP(1);
            READ_OR_ERROR(reader_u8, &hit_count);
            monster_count = hit_count >> 4;
            hit_count &= 0xF;
            READ_OR_ERROR(reader_u32, &skill);
            SKIP(8);
            READ_OR_ERROR(reader_u8, &display);
            READ_OR_ERROR(reader_u8, &direction);
            READ_OR_ERROR(reader_u8, &stance);
            SKIP(1);
            READ_OR_ERROR(reader_u8, &speed);
            SKIP(4);
            for (int8_t i = 0; i < monster_count; i++) {
                READ_OR_ERROR(reader_u32, &oids[i]);
                SKIP(14);

                for (int8_t j = 0; j < hit_count; j++)
                    READ_OR_ERROR(reader_i32, &damage[i * hit_count + j]);


                SKIP(4);
            }
            SKIP(4);
            READER_END();

            int8_t level = 0;
            if (skill != 0) {
                if (!user_use_skill(user, skill, &level, NULL))
                    break;
            }

            uint32_t killed[16];
            size_t kill_count;
            if (!room_member_magic_attack(room, member, monster_count, hit_count, skill, level, display, direction, stance, speed, oids, damage, &kill_count, killed))
                break;

            bool leveled;
            if (!user_kill_monsters(user, kill_count, killed, &leveled))
                break;

            if (leveled)
                room_member_level_up(room, member);
        }
        break;

        case RECEIVE_OPCODE_TAKE_DAMAGE: {
            uint8_t skill;
            int32_t damage;
            uint32_t id;
            uint32_t oid;
            uint8_t direction;
            READER_BEGIN(packet_len, packet);
            SKIP(4);
            READ_OR_ERROR(reader_u8, &skill);
            SKIP(1); // Element
            READ_OR_ERROR(reader_i32, &damage);
            if (skill != (uint8_t)-3 && skill != (uint8_t)-4) {
                READ_OR_ERROR(reader_u32, &id);
                READ_OR_ERROR(reader_u32, &oid);
                READ_OR_ERROR(reader_u8, &direction);
            }

            READER_END();

            if (skill != (uint8_t)-3 && skill != (uint8_t)-4) {
                if (!room_monster_exists(room, oid, id))
                    break;

                user_take_damage(user, -damage);
                room_member_take_damage(room, member, skill, damage, id, direction);
            }
        }
        break;

        case RECEIVE_OPCODE_CHAT: {
            uint16_t str_len = 80;
            char string[81];
            uint8_t show;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_sized_string, &str_len, string);
            if (string[0] != '/')
                READ_OR_ERROR(reader_u8, &show);
            READER_END();

            if (string[0] == '!') {
                if (!strncmp(string + 1, "autopickup", str_len - 1)) {
                    //client_toggle_auto_pickup(client);
                    //if (client_is_auto_pickup_enabled(client)) {
                    //    map_for_each_drop(session->room->map, pick_up, session);
                    //    //map_pick_up_all(session->room->map, session->player);
                    //}
                } else if (!strncmp(string + 1, "killall", str_len - 1)) {
                    //map_for_each_monster(session->room->map, kill_monster, session);
                    //map_kill_all(session->room->map);
                } else {
                    // // strtok() must take a nul-terminated string
                    // string[str_len] = '\0';
                    // char *save;
                    // char *token = strtok_r(string + 1, " ", &save);
                    // if (!strcmp(token, "map")) {
                    //     token = strtok_r(NULL, " ", &save);
                    //     const struct MapInfo *info = wz_get_map(strtol(token, NULL, 10));
                    //     if (info != NULL) {
                    //         client_warp(client, info->id, 0);
                    //     } else {
                    //         client_message(client, "Requested map doesn't exist");
                    //     }
                    // } else if (!strcmp(token, "spawn")) {
                    //     token = strtok_r(NULL, " ", &save);
                    //     const struct Point *pos = map_player_pos(session->player);
                    //     map_spawn(session->room->map, strtol(token, NULL, 10), *pos);
                    // }
                }
            } else if (string[0] != '/') {
                room_member_chat(room, member, str_len, string, show);
            }
        }
        break;

        case RECEIVE_OPCODE_FACIAL_EXPRESSION: {
            uint32_t emote;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u32, &emote);
            READER_END();

            // TODO: Check emote legality
            room_member_emote(room, member, emote);
        }
        break;

        case RECEIVE_OPCODE_NPC_TALK: {
            if (member == NULL)
                break;

            uint32_t oid;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u32, &oid);
            READER_END();

            uint32_t id = room_get_npc(room, oid);
            if (id == (uint32_t)-1) {
                session_shutdown(session);
                break;
            }

            user_talk_npc(user, id);
        }
        break;

        case RECEIVE_OPCODE_DIALOGUE: {
            uint8_t last;
            uint8_t action;
            uint32_t selection = -1;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u8, &last);
            READ_OR_ERROR(reader_u8, &action);
            if (last == NPC_DIALOGUE_TYPE_SIMPLE || last == 3) {
                if (READER_AVAILABLE() >= 4) {
                    READ_OR_ERROR(reader_u32, &selection);
                } else if (READER_AVAILABLE() > 0) {
                    uint8_t sel_u8;
                    READ_OR_ERROR(reader_u8, &sel_u8);
                    selection = sel_u8;
                }
            }

            READER_END();

            user_script_cont(user, last, action, selection);
        }
        break;

        case RECEIVE_OPCODE_SHOP_ACTION: {
            uint8_t action;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u8, &action);
            switch (action) {
            case 0: { // Buy
                uint16_t slot;
                uint32_t id;
                int16_t quantity;
                int32_t price;
                READ_OR_ERROR(reader_u16, &slot);
                READ_OR_ERROR(reader_u32, &id);
                READ_OR_ERROR(reader_i16, &quantity);
                READ_OR_ERROR(reader_i32, &price);
                user_buy(user, slot, id, quantity, price);
            }
                break;

            case 1: { // Sell
                uint16_t slot;
                uint32_t id;
                int16_t quantity;
                READ_OR_ERROR(reader_u16, &slot);
                READ_OR_ERROR(reader_u32, &id);
                READ_OR_ERROR(reader_i16, &quantity);
                user_sell(user, slot, id, quantity);
            }
            break;

            case 2: { // Recharge
                uint16_t slot;
                READ_OR_ERROR(reader_u16, &slot);
                user_recharge(user, slot);
            }
            break;

            case 3: // Leave
            user_close_shop(user);
            break;

            default:
            session_shutdown(session);
            }
            READER_END();
        }
        break;

        case RECEIVE_OPCODE_ITEM_MOVE: {
            uint8_t inventory;
            int16_t src;
            int16_t dst;
            int16_t quantity;
            READER_BEGIN(packet_len, packet);
            SKIP(4);
            READ_OR_ERROR(reader_u8, &inventory);
            READ_OR_ERROR(reader_i16, &src);
            READ_OR_ERROR(reader_i16, &dst);
            READ_OR_ERROR(reader_i16, &quantity);
            READER_END();

            if (dst == 0) {
                if (quantity <= 0) {
                    session_shutdown(session);
                    break;
                }

                struct Drop drop;

                if (inventory != 1) {
                    drop.type = DROP_TYPE_ITEM;
                    if (!user_remove_item(user, inventory, src, quantity, &drop.item))
                        break;
                } else {
                    if (quantity > 1)
                        session_shutdown(session);

                    drop.type = DROP_TYPE_EQUIP;
                    if (src < 0) {
                        if (!user_remove_equip(user, true, -src, &drop.equip))
                            break;
                    } else {
                        if (!user_remove_equip(user, false, src, &drop.equip))
                            break;
                    }
                }

                room_member_drop(room, member, &drop);
            } else {
                if (quantity != -1)
                    session_shutdown(session);

                user_move_item(user, inventory, src, dst);
            }
        }
        break;

        case RECEIVE_OPCODE_ITEM_USE: {
            uint16_t slot;
            uint32_t item_id;
            READER_BEGIN(packet_len, packet);
            SKIP(4);
            READ_OR_ERROR(reader_u16, &slot);
            READ_OR_ERROR(reader_u32, &item_id);
            READER_END();

            user_use_item(user, slot, item_id);
        }
        break;

        case RECEIVE_OPCODE_AP_ASSIGN: {
            uint32_t stat;
            READER_BEGIN(packet_len, packet);
            SKIP(4);
            READ_OR_ERROR(reader_u32, &stat);
            READER_END();

            user_assign_stat(user, stat);
        }
        break;

        case RECEIVE_OPCODE_AUTO_AP_ASSIGN:{
            uint32_t stat1;
            uint32_t val1;
            uint32_t stat2;
            uint32_t val2;
            READER_BEGIN(packet_len, packet);
            SKIP(8);
            READ_OR_ERROR(reader_u32, &stat1);
            READ_OR_ERROR(reader_u32, &val1);
            READ_OR_ERROR(reader_u32, &stat2);
            READ_OR_ERROR(reader_u32, &val2);
            READER_END();

            if (!user_assign_stat(user, stat1))
                break;

            user_assign_stat(user, stat2);
        }
        break;

        case RECEIVE_OPCODE_HEAL_OVER_TIME: {
            int16_t hp, mp;
            READER_BEGIN(packet_len, packet);
            SKIP(8);
            READ_OR_ERROR(reader_i16, &hp);
            READ_OR_ERROR(reader_i16, &mp);
            SKIP(1);
            READER_END();

            // TODO: Check if the client is allowed to heal this much HP/MP
            user_adjust_hp(user, hp);
            user_adjust_mp(user, mp);
        }
        break;

        case RECEIVE_OPCODE_SP_ASSIGN: {
            uint32_t id;
            READER_BEGIN(packet_len, packet);
            SKIP(4);
            READ_OR_ERROR(reader_u32, &id);
            READER_END();

            user_assign_sp(user, id);
        }
        break;

        case RECEIVE_OPCODE_MESO_DROP: {
            if (member == NULL)
                break;

            int32_t amount;
            READER_BEGIN(packet_len, packet);
            SKIP(4);
            READ_OR_ERROR(reader_i32, &amount);
            READER_END();

            if (amount < 10 || amount > 50000)
                session_shutdown(session);

            if (!user_gain_meso(user, -amount, false, false))
                break;

            struct Drop drop = {
                .type = DROP_TYPE_MESO,
                .meso = amount,
            };
            room_member_drop(room, member, &drop);
        }
        break;

        case RECEIVE_OPCODE_SCRIPTED_PORTAL: {
            if (member == NULL)
                break;

            uint16_t len = 17;
            char str[17];
            READER_BEGIN(packet_len, packet);
            SKIP(1);
            READ_OR_ERROR(reader_sized_string, &len, str);
            SKIP(4);
            READER_END();

            str[len] = '\0';
            const struct PortalInfo *info = wz_get_portal_info_by_name(room_id(room), str);
            if (info == NULL) {
                // The client can spam enter portal which will cause a search for the portal in the destination map
                break;
            }

            user_portal_script(user, info->script);
        }
        break;

        case RECEIVE_OPCODE_QUEST_ACTION: {
            if (member == NULL)
                break;

            uint8_t action;
            uint16_t qid;
            uint32_t npc;
            int16_t x, y;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u8, &action);
            READ_OR_ERROR(reader_u16, &qid);
            switch (action) {
            case 0: {
                uint32_t item_id;
                SKIP(4);
                READ_OR_ERROR(reader_u32, &item_id);
                user_regain_quest_item(user, qid, item_id);
            }
                break;

            case 1: {
                READ_OR_ERROR(reader_u32, &npc);
                if (READER_AVAILABLE() == 4) {
                    READ_OR_ERROR(reader_i16, &x);
                    READ_OR_ERROR(reader_i16, &y);
                }
                uint32_t *ids;
                ssize_t count = user_start_quest(user, qid, npc, false, &ids);
                if (count == -1)
                    break;

                if (count != 0) {
                    room_member_add_quest_items(member, count, ids);
                    free(ids);
                }
            }
            break;

            case 2: {
                READ_OR_ERROR(reader_u32, &npc);
                if (READER_AVAILABLE() == 4) {
                    READ_OR_ERROR(reader_i16, &x);
                    READ_OR_ERROR(reader_i16, &y);
                }
                //uint8_t selection;
                if (!user_end_quest(user, qid, npc, false))
                    break;;

                room_member_effect(room, member, 0x09);
            }
            break;

            case 3: {
                user_forfeit_quest(user, qid);
            }
            break;

            case 4: {
                READ_OR_ERROR(reader_u32, &npc);
                if (READER_AVAILABLE() == 4) {
                    READ_OR_ERROR(reader_i16, &x);
                    READ_OR_ERROR(reader_i16, &y);
                }
                uint32_t *ids;
                ssize_t count = user_start_quest(user, qid, npc, true, &ids);
                if (count == -1)
                    break;

                if (count != 0) {
                    room_member_add_quest_items(member, count, ids);
                    free(ids);
                }
            }
            break;

            case 5: {
                READ_OR_ERROR(reader_u32, &npc);
                if (READER_AVAILABLE() == 4) {
                    SKIP(4);
                    //READ_OR_ERROR(reader_i16, &x);
                    //READ_OR_ERROR(reader_i16, &y);
                }
                user_end_quest(user, qid, npc, true);
            }
            break;

            default:
            session_shutdown(session);
            }
            READER_END();
        }
        break;

        /*case RECEIVE_OPCODE_PARTY_OP: {
          uint8_t op;
          READER_BEGIN(size, packet);
          READ_OR_ERROR(reader_u8, &op);
          switch (op) {
          case 1: {
          client_create_party(client);
          }
          break;

          case 3: {
          }
          break;

          case 4: {
          uint16_t name_len = CHARACTER_MAX_NAME_LENGTH;
          char name[CHARACTER_MAX_NAME_LENGTH];
          READ_OR_ERROR(reader_sized_string, &name_len, name);
          client_invite_to_party(client, name_len, name);
          }
          break;
          }
          READER_END();
          }
          break;

          case RECEIVE_OPCODE_PARTY_DENY: {
          uint16_t name_len = CHARACTER_MAX_NAME_LENGTH;
          char name[CHARACTER_MAX_NAME_LENGTH];
          READER_BEGIN(size, packet);
          SKIP(1);
          READ_OR_ERROR(reader_sized_string, &name_len, name);
          READER_END();

          client_reject_party_invitaion(client, name_len, name);
          }
          break;*/

        case RECEIVE_OPCODE_KEYMAP_CHANGE: {
            if (member == NULL)
                break;

            uint32_t mode;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u32, &mode);
            if (mode == 0) {
                uint32_t changeCount;
                READ_OR_ERROR(reader_u32, &changeCount);
                for (size_t i = 0; i < changeCount; i++) {
                    uint32_t key;
                    uint8_t type;
                    uint32_t action;
                    READ_OR_ERROR(reader_u32, &key);
                    READ_OR_ERROR(reader_u8, &type);
                    READ_OR_ERROR(reader_u32, &action);
                    if (type > 0) {
                        if (type == 1) {
                            if (!user_add_skill_key(user, key, action))
                                break;
                        } else {
                            if (!user_add_key(user, key, type, action))
                                break;
                        }
                    } else {
                        if (!user_remove_key(user, key, action))
                            break;
                    }
                }
            } else if (mode == 1) { // Auto-HP
            } else if (mode == 2) { // Auto-MP
            } else {
                // Illegal mode
                break;
            }
            READER_END();
        }
        break;

        case 0x00B7: {
        }
        break;

        case RECEIVE_OPCODE_MONSTER_MOVE: {
            if (member == NULL)
                break;

            uint32_t oid;
            uint16_t moveid;
            uint8_t activity;
            uint8_t skill_id;
            uint8_t skill_level;
            uint16_t option;
            int16_t x, y;
            uint16_t fh;
            uint8_t stance;
            size_t len = 5; // startx (uint16_t), starty (uint16_t), count (uint8_t)
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u32, &oid);
            READ_OR_ERROR(reader_u16, &moveid);
            SKIP(1);
            READ_OR_ERROR(reader_u8, &activity);
            READ_OR_ERROR(reader_u8, &skill_id);
            READ_OR_ERROR(reader_u8, &skill_level);
            READ_OR_ERROR(reader_u16, &option);
            SKIP(17);
            uint8_t count;
            READ_OR_ERROR(reader_u8, &count);
            for (uint8_t i = 0; i < count; i++) {
                uint8_t command;
                READ_OR_ERROR(reader_u8, &command);
                len++;
                switch (command) {
                case 0: case 5: case 17:
                    READ_OR_ERROR(reader_i16, &x);
                    READ_OR_ERROR(reader_i16, &y);
                    SKIP(4);
                    READ_OR_ERROR(reader_u16, &fh);
                    READ_OR_ERROR(reader_u8, &stance);

                    SKIP(2);
                    len += 13;
                break;

                case 1: case 2: case 6: case 12: case 13: case 16: case 18: case 19: case 20: case 22:
                    SKIP(7);
                    len += 7;
                break;

                case 3: case 4: case 7: case 8: case 9: case 11: case 14:
                    SKIP(9);
                    len += 9;
                break;

                case 10:
                    SKIP(1);
                    len += 1;
                break;

                case 15:
                    SKIP(15);
                    len += 15;
                break;

                case 21:
                    SKIP(3);
                    len += 3;
                break;
                }
            }
            SKIP(9);
            READER_END();

            room_member_move_monster(room, member, oid, moveid, activity, skill_id, skill_level, option, len, packet + 25, x, y, fh, stance);
        }
        break;

        case RECEIVE_OPCODE_NPC_MOVE: {
            if (member == NULL)
                break;

            READER_BEGIN(packet_len, packet);
            if (packet_len == 6) {
                uint8_t data[6];
                uint8_t out[8];
                READ_OR_ERROR(reader_array, 6, data);
                npc_action_packet(6, data, out);
                session_write(session, 8, out);
            } else if (packet_len > 9) {
                uint8_t data[packet_len - 9];
                uint8_t out[packet_len - 7];
                READ_OR_ERROR(reader_array, packet_len - 9, data);
                npc_action_packet(packet_len - 9, data, out);
                session_write(session, packet_len - 7, out);
            } else {
                session_shutdown(session);
            }

            READER_END();
        }
        break;

        case RECEIVE_OPCODE_PICKUP: {
            uint32_t oid;
            READER_BEGIN(packet_len, packet);
            SKIP(9);
            READ_OR_ERROR(reader_u32, &oid);
            READER_END();

            struct Drop drop;
            if (!room_member_get_drop(room, member, oid, &drop))
                break;

            switch (drop.type) {
            case DROP_TYPE_MESO:
                if (!user_gain_meso(user, drop.meso, true, false))
                    break;
            break;
            case DROP_TYPE_ITEM: {
                // TODO: Check if there are any consumables that can give the player EXP
                // as if there is such item the effect can be 0 which indicates a level up
                uint8_t effect = 0;
                if (!user_gain_inventory_item(user, &drop.item, &effect))
                    break;

                if (effect != 0)
                    room_member_effect(room, member, effect);
            }
            break;
            case DROP_TYPE_EQUIP:
                if (!user_gain_equipment(user, &drop.equip))
                    break;
            break;
            }

            room_member_pick_up_drop(room, member, oid);
        }
        break;

        case RECEIVE_OPCODE_REACTOR_HIT: {
            uint32_t oid;
            uint8_t stance;
            READER_BEGIN(packet_len, packet);
            READ_OR_ERROR(reader_u32, &oid);
            SKIP(4); // Position
            READ_OR_ERROR(reader_u8, &stance);
            SKIP(5);
            SKIP(4); // skill ID
            READER_END();

            room_member_hit_reactor(room, member, oid, stance);
        }
        break;

        case RECEIVE_OPCODE_MAP_TRANSFER_COMPLETE: {
            READER_BEGIN(packet_len, packet);
            READER_END();

            struct IdRoom *id_room = hash_set_u32_get(thread_ctx->rooms, actor->targetMap);
            // This can happen when room creation has failed during transfer
            if (id_room == NULL) {
                session_shutdown(session);
                break;
            }

            const struct PortalInfo *portal = &wz_get_map(actor->targetMap)->portals[actor->targetPortal];
            actor->player->x = portal->x;
            actor->player->y = portal->y;
            actor->room = id_room->room;
            actor->member = room_join(actor->room, session, actor->player, actor->questItems, actor->server->reactorManager);
            if (actor->member == NULL) {
                session_shutdown(session);
                break;
            }

            room = actor->room;
            member = actor->member;

            // Forced stat reset
            session_write(session, 2, (uint8_t[]) { 0x23, 0x00 }); // Forced stat reset

            if (!user_script_cont(user, 0, 0, 0))
                break;

            //user_launch_map_script(user, room_id(room));
        }
        break;

        default:
        break;
        }

        packet += packet_len;
        len -= 4 + 2 + packet_len;
    }

    memcpy(actor->buf, actor->buf + (actor->len - len), len);
    actor->len = len;
    if (room != NULL)
        worker_read(worker, actor->fd, actor->buf + actor->len, BUF_LEN - actor->len, actor->len != 0, handle_packet, actor);
}

static void join_room(struct Worker *worker, void *user_data);

static void on_writes_flushed(struct Session *session, void *user_data)
{
    struct Actor *actor = user_data;
    ssize_t thread = room_thread_coordinator_ref(actor->server->coordinator, actor->targetMap);
    if (thread == -1) {
        free(actor);
        session_shutdown(session);
        return;
    }

    assert(thread >= 0);

    worker_command(thread_pool_get_worker(actor->server->pool, thread), join_room, actor);
}

static void join_room(struct Worker *worker, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Session *session = actor->session;

    session_set_worker(session, worker);

    struct IdRoom *id_room = hash_set_u32_get(thread_ctx->rooms, actor->targetMap);
    if (id_room == NULL) {
        struct IdRoom room = { .id = actor->targetMap, room_create(worker, actor->server->eventManager, actor->targetMap) };
        if (room.room == NULL)
            ; // TODO
        hash_set_u32_insert(thread_ctx->rooms, &room);
        id_room = hash_set_u32_get(thread_ctx->rooms, actor->targetMap);
    }

    user_change_map(actor->user, actor->targetMap, actor->targetPortal);
    actor->room = id_room->room;
    worker_read(worker, actor->fd, actor->buf + actor->len, BUF_LEN - actor->len, actor->len != 0, handle_packet, actor);
}

static void join_room_new(struct Worker *worker, void *user_data);

static void on_read_canceled(struct Worker *worker, int status, void *user_data)
{
    struct Actor *actor = user_data;
    if (status == -ENOENT || status == -EALREADY)
        return;

    // Can't dereference ctx before here as it can be invalid in case of ENOENT
    struct Session *session = actor->session;

    ssize_t thread = room_thread_coordinator_ref(actor->server->coordinator, actor->targetMap);
    if (thread == -1) {
        user_destroy(actor->user);
        actor->user = NULL;
        session_shutdown(session);
        return;
    }

    worker_command(thread_pool_get_worker(actor->server->pool, thread), join_room_new, actor);
}

static void join_room_new(struct Worker *worker, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Session *session = actor->session;

    session_set_worker(session, worker);

    struct IdRoom *id_room = hash_set_u32_get(thread_ctx->rooms, actor->targetMap);
    if (id_room == NULL) {
        struct Room *room = room_create(worker, actor->server->eventManager, actor->targetMap);
        if (room == NULL)
            ;

        struct IdRoom id_room = {
            .id = actor->targetMap,
            .room = room
        };

        hash_set_u32_insert(thread_ctx->rooms, &id_room);
    }

    user_new_map(actor->user);
    worker_read(worker, actor->fd, actor->buf, BUF_LEN, false, handle_packet, actor);
}

static void add_quest(void *data, void *ctx);
static void add_progress(void *data, void *ctx);
static void add_quest_info(void *data, void *ctx);
static void add_completed_quest(void *data, void *ctx);
static void add_skill(void *data, void *ctx);
static void add_monster_book_entry(void *data, void *ctx);

static void on_flush_database_unlocked(struct Worker *worker, int, void *user_data);

static void flush_character(struct Worker *worker, int status, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Session *session = actor->session;
    actor->efd = database_connection_lock(thread_ctx->conn);
    if (actor->efd >= -1) {
        if (actor->efd == -1)
            session_shutdown(session);
        else
            worker_poll(worker, actor->efd, POLLIN, on_flush_database_unlocked, actor);
        return;
    }

    on_flush_database_unlocked(worker, 0, actor);
}

static void continue_allocate_ids(struct Worker *worker, int, void *);

static void on_flush_database_unlocked(struct Worker *worker, int status, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Character *chr = user_get_character(actor->user);
    struct RequestParams params_ = {
        .type = DATABASE_REQUEST_TYPE_ALLOCATE_IDS,
        .allocateIds = {
            .id = chr->id,
            .accountId = chr->accountId,
            .itemCount = 0,
            .equippedCount = 0,
            .equipCount = 0,
            .storageItemCount = 0,
            .storageEquipCount = 0
        },
    };

    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < chr->inventory[i].slotCount; j++) {
            if (!chr->inventory[i].items[j].isEmpty && chr->inventory[i].items[j].item.item.id == 0) {
                params_.allocateIds.items[params_.allocateIds.itemCount] = chr->inventory[i].items[j].item.item.itemId;
                params_.allocateIds.itemCount++;
            }
        }
    }

    for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (!chr->equippedEquipment[i].isEmpty && chr->equippedEquipment[i].equip.id == 0) {
            params_.allocateIds.equippedEquipment[params_.allocateIds.equippedCount].id =
                chr->equippedEquipment[i].equip.item.id;
            params_.allocateIds.equippedEquipment[params_.allocateIds.equippedCount].equipId =
                chr->equippedEquipment[i].equip.equipId;
            params_.allocateIds.equippedEquipment[params_.allocateIds.equippedCount].itemId =
                chr->equippedEquipment[i].equip.item.itemId;
            params_.allocateIds.equippedCount++;
        }
    }

    for (size_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        if (!chr->equipmentInventory.items[i].isEmpty && chr->equipmentInventory.items[i].equip.id == 0) {
            params_.allocateIds.equipmentInventory[params_.allocateIds.equipCount].id =
                chr->equipmentInventory.items[i].equip.item.id;
            params_.allocateIds.equipmentInventory[params_.allocateIds.equipCount].equipId =
                chr->equipmentInventory.items[i].equip.equipId;
            params_.allocateIds.equipmentInventory[params_.allocateIds.equipCount].itemId =
                chr->equipmentInventory.items[i].equip.item.itemId;
            params_.allocateIds.equipCount++;
        }
    }

    actor->req = database_request_create(thread_ctx->conn, &params_);
    if (actor->req == NULL) {
        database_connection_unlock(thread_ctx->conn);
        return;
    }

    continue_allocate_ids(worker, 0, actor);
}

static void continue_flush_character(struct Worker *worker, int status, void *user_data);

static void continue_allocate_ids(struct Worker *worker, int status, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;
    struct Session *session = actor->session;
    struct Character *chr = user_get_character(actor->user);
    status = database_request_execute(actor->req, status);
    if (status != 0) {
        if (status > 0) {
            worker_poll(worker, database_connection_get_fd(thread_ctx->conn), status, continue_allocate_ids, actor);
        } else if (status < 0) {
            database_request_destroy(actor->req);
            database_connection_unlock(thread_ctx->conn);
            actor->req = NULL;
            session_destroy(session);
        }
        return;
    }

    const struct RequestParams *params = database_request_get_params(actor->req);
    const union DatabaseResult *res = database_request_result(actor->req);
    size_t count = 0;

    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < chr->inventory[i].slotCount; j++) {
            if (!chr->inventory[i].items[j].isEmpty && chr->inventory[i].items[j].item.item.id == 0) {
                chr->inventory[i].items[j].item.item.id = res->allocateIds.items[count];
                count++;
            }
        }
    }

    count = 0;
    for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (!chr->equippedEquipment[i].isEmpty && chr->equippedEquipment[i].equip.id == 0) {
            chr->equippedEquipment[i].equip.item.id = params->allocateIds.equippedEquipment[count].id;
            chr->equippedEquipment[i].equip.equipId = params->allocateIds.equippedEquipment[count].equipId;
            chr->equippedEquipment[i].equip.id = res->allocateIds.equippedEquipment[count];
            count++;
        }
    }

    count = 0;
    for (size_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        if (!chr->equipmentInventory.items[i].isEmpty && chr->equipmentInventory.items[i].equip.id == 0) {
            chr->equipmentInventory.items[i].equip.item.id = params->allocateIds.equipmentInventory[count].id;
            chr->equipmentInventory.items[i].equip.equipId = params->allocateIds.equipmentInventory[count].equipId;
            chr->equipmentInventory.items[i].equip.id = res->allocateIds.equipmentInventory[count];
            count++;
        }
    }

    database_request_destroy(actor->req);

    struct RequestParams params_ = {
        .type = DATABASE_REQUEST_TYPE_UPDATE_CHARACTER,
    };

    params_.updateCharacter.keyMap = NULL;
    params_.updateCharacter.monsterBook = NULL;
    params_.updateCharacter.skills = NULL;
    params_.updateCharacter.completedQuests = NULL;
    params_.updateCharacter.questInfos = NULL;
    params_.updateCharacter.progresses = NULL;
    params_.updateCharacter.quests = NULL;

    params_.type = DATABASE_REQUEST_TYPE_UPDATE_CHARACTER;
    params_.updateCharacter.id = chr->id;
    params_.updateCharacter.accountId = chr->accountId;
    params_.updateCharacter.map = wz_get_map_forced_return(chr->map);
    params_.updateCharacter.spawnPoint = chr->spawnPoint;
    params_.updateCharacter.job = chr->job;
    params_.updateCharacter.level = chr->level;
    params_.updateCharacter.exp = chr->exp;
    params_.updateCharacter.maxHp = chr->maxHp;
    params_.updateCharacter.hp = chr->hp;
    params_.updateCharacter.maxMp = chr->maxMp;
    params_.updateCharacter.mp = chr->mp;
    params_.updateCharacter.str = chr->str;
    params_.updateCharacter.dex = chr->dex;
    params_.updateCharacter.int_ = chr->int_;
    params_.updateCharacter.luk = chr->luk;
    params_.updateCharacter.ap = chr->ap;
    params_.updateCharacter.sp = chr->sp;
    params_.updateCharacter.fame = chr->fame;
    params_.updateCharacter.skin = chr->skin;
    params_.updateCharacter.face = chr->face;
    params_.updateCharacter.hair = chr->hair;
    params_.updateCharacter.mesos = chr->mesos;
    params_.updateCharacter.equipSlots = chr->equipmentInventory.slotCount;
    params_.updateCharacter.useSlots = chr->inventory[0].slotCount;
    params_.updateCharacter.setupSlots = chr->inventory[1].slotCount;
    params_.updateCharacter.etcSlots = chr->inventory[2].slotCount;
    params_.updateCharacter.equippedCount = 0;
    params_.updateCharacter.equipCount = 0;
    params_.updateCharacter.itemCount = 0;

    for (size_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (!chr->equippedEquipment[i].isEmpty) {
            struct Equipment *src = &chr->equippedEquipment[i].equip;
            struct DatabaseCharacterEquipment *dst =
                &params_.updateCharacter.equippedEquipment[params_.updateCharacter.equippedCount];

            dst->id = src->id;
            dst->equip.id = src->equipId;
            dst->equip.item.id = src->item.id;
            dst->equip.item.itemId = src->item.itemId;
            dst->equip.item.flags = src->item.flags;
            dst->equip.item.ownerLength = src->item.ownerLength;
            memcpy(dst->equip.item.owner, src->item.owner,
                    src->item.ownerLength);
            dst->equip.item.giverLength = src->item.giftFromLength;
            memcpy(dst->equip.item.giver, src->item.giftFrom,
                    src->item.giftFromLength);


            dst->equip.level = src->level;
            dst->equip.slots = src->slots;
            dst->equip.str = src->str;
            dst->equip.dex = src->dex;
            dst->equip.int_ = src->int_;
            dst->equip.luk = src->luk;
            dst->equip.hp = src->hp;
            dst->equip.mp = src->mp;
            dst->equip.atk = src->atk;
            dst->equip.matk = src->matk;
            dst->equip.def = src->def;
            dst->equip.mdef = src->mdef;
            dst->equip.acc = src->acc;
            dst->equip.avoid = src->avoid;
            dst->equip.speed = src->speed;
            dst->equip.jump = src->jump;

            params_.updateCharacter.equippedCount++;
        }
    }

    for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        if (!chr->equipmentInventory.items[i].isEmpty) {
            struct Equipment *src = &chr->equipmentInventory.items[i].equip;
            struct DatabaseCharacterEquipment *dst =
                &params_.updateCharacter.equipmentInventory[params_.updateCharacter.equipCount].equip;

            dst->id = src->id;
            dst->equip.id = src->equipId;
            params_.updateCharacter.equipmentInventory[params_.updateCharacter.equipCount].slot = i;
            dst->equip.item.id = src->item.id;
            dst->equip.item.itemId = src->item.itemId;
            dst->equip.item.flags = src->item.flags;
            dst->equip.item.ownerLength = src->item.ownerLength;
            memcpy(dst->equip.item.owner, src->item.owner, src->item.ownerLength);
            dst->equip.item.giverLength = src->item.giftFromLength;
            memcpy(dst->equip.item.giver, src->item.giftFrom, src->item.giftFromLength);

            dst->equip.level = src->level;
            dst->equip.slots = src->slots;
            dst->equip.str = src->str;
            dst->equip.dex = src->dex;
            dst->equip.int_ = src->int_;
            dst->equip.luk = src->luk;
            dst->equip.hp = src->hp;
            dst->equip.mp = src->mp;
            dst->equip.atk = src->atk;
            dst->equip.matk = src->matk;
            dst->equip.def = src->def;
            dst->equip.mdef = src->mdef;
            dst->equip.acc = src->acc;
            dst->equip.avoid = src->avoid;
            dst->equip.speed = src->speed;
            dst->equip.jump = src->jump;

            params_.updateCharacter.equipCount++;
        }
    }

    for (uint8_t inv = 0; inv < 4; inv++) {
        for (uint8_t i = 0; i < chr->inventory[inv].slotCount; i++) {
            if (!chr->inventory[inv].items[i].isEmpty) {
                struct Item *src = &chr->inventory[inv].items[i].item.item;
                struct DatabaseItem *dst =
                    &params_.updateCharacter.inventoryItems[params_.updateCharacter.itemCount].item;
                params_.updateCharacter.inventoryItems[params_.updateCharacter.itemCount].slot = i;
                params_.updateCharacter.inventoryItems[params_.updateCharacter.itemCount].count =
                    chr->inventory[inv].items[i].item.quantity;

                dst->id = src->id;
                dst->itemId = src->itemId;
                dst->flags = src->flags;
                dst->ownerLength = src->ownerLength;
                memcpy(dst->owner, src->owner, src->ownerLength);
                dst->giverLength = src->giftFromLength;
                memcpy(dst->giver, src->giftFrom, src->giftFromLength);

                params_.updateCharacter.itemCount++;
            }
        }
    }

    params_.updateCharacter.quests = malloc(hash_set_u16_size(chr->quests) * sizeof(uint16_t));
    if (params_.updateCharacter.quests == NULL) {
        database_connection_unlock(thread_ctx->conn);
        session_shutdown(session);
        return;
    }

    {
        struct {
            uint16_t *quests;
            size_t currentQuest;
            size_t progressCount;
        } ctx = { params_.updateCharacter.quests, 0, 0 };
        hash_set_u16_foreach(chr->quests, add_quest, &ctx);

        params_.updateCharacter.questCount = ctx.currentQuest;

        params_.updateCharacter.progresses = malloc(ctx.progressCount * sizeof(struct DatabaseProgress));
        if (params_.updateCharacter.progresses == NULL) {
            database_connection_unlock(thread_ctx->conn);
            free(params_.updateCharacter.quests);
            session_shutdown(session);
            return;
        }
    }

    struct {
        struct DatabaseProgress *progresses;
        size_t currentProgress;
    } ctx2 = { params_.updateCharacter.progresses, 0 };
    hash_set_u16_foreach(chr->quests, add_progress, &ctx2);

    params_.updateCharacter.progressCount = ctx2.currentProgress;

    params_.updateCharacter.questInfos = malloc(hash_set_u16_size(chr->questInfos) * sizeof(struct DatabaseInfoProgress));
    if (params_.updateCharacter.questInfos == NULL) {
        database_connection_unlock(thread_ctx->conn);
        free(params_.updateCharacter.progresses);
        free(params_.updateCharacter.quests);
        session_shutdown(session);
    }

    struct {
        struct DatabaseInfoProgress *infos;
        size_t currentInfo;
    } ctx3 = { params_.updateCharacter.questInfos, 0 };

    hash_set_u16_foreach(chr->questInfos, add_quest_info, &ctx3);

    params_.updateCharacter.questInfoCount = ctx3.currentInfo;

    params_.updateCharacter.completedQuests = malloc(hash_set_u16_size(chr->completedQuests) * sizeof(struct DatabaseCompletedQuest));
    if (params_.updateCharacter.completedQuests == NULL) {
        database_connection_unlock(thread_ctx->conn);
        free(params_.updateCharacter.questInfos);
        free(params_.updateCharacter.progresses);
        free(params_.updateCharacter.quests);
        session_shutdown(session);
    }

    struct {
        struct DatabaseCompletedQuest *quests;
        size_t currentQuest;
    } ctx4 = { params_.updateCharacter.completedQuests, 0 };
    hash_set_u16_foreach(chr->completedQuests, add_completed_quest, &ctx4);

    params_.updateCharacter.completedQuestCount = ctx4.currentQuest;

    params_.updateCharacter.skills = malloc(hash_set_u32_size(chr->skills) * sizeof(struct DatabaseSkill));
    if (params_.updateCharacter.completedQuests == NULL) {
        database_connection_unlock(thread_ctx->conn);
        free(params_.updateCharacter.completedQuests);
        free(params_.updateCharacter.questInfos);
        free(params_.updateCharacter.progresses);
        free(params_.updateCharacter.quests);
        session_shutdown(session);
    }

    struct {
        struct DatabaseSkill *skills;
        size_t currentSkill;
    } ctx5 = { params_.updateCharacter.skills, 0 };
    hash_set_u32_foreach(chr->skills, add_skill, &ctx5);

    params_.updateCharacter.skillCount = ctx5.currentSkill;

    params_.updateCharacter.monsterBook = malloc(hash_set_u32_size(chr->monsterBook) * sizeof(struct DatabaseMonsterBookEntry));
    if (params_.updateCharacter.monsterBook == NULL) {
        database_connection_unlock(thread_ctx->conn);
        free(params_.updateCharacter.skills);
        free(params_.updateCharacter.completedQuests);
        free(params_.updateCharacter.questInfos);
        free(params_.updateCharacter.progresses);
        free(params_.updateCharacter.quests);
        session_shutdown(session);
    }

    struct {
        struct DatabaseMonsterBookEntry *monsterBook;
        size_t currentEntry;
    } ctx6 = { params_.updateCharacter.monsterBook, 0 };
    hash_set_u32_foreach(chr->monsterBook, add_monster_book_entry, &ctx6);

    params_.updateCharacter.monsterBookEntryCount = ctx6.currentEntry;

    uint8_t key_count = 0;
    for (uint8_t i = 0; i < KEYMAP_MAX_KEYS; i++) {
        if (chr->keyMap[i].type != 0)
            key_count++;
    }

    params_.updateCharacter.keyMap = malloc(key_count * sizeof(struct DatabaseKeyMapEntry));
    if (params_.updateCharacter.keyMap == NULL) {
        database_connection_unlock(thread_ctx->conn);
        free(params_.updateCharacter.monsterBook);
        free(params_.updateCharacter.skills);
        free(params_.updateCharacter.completedQuests);
        free(params_.updateCharacter.questInfos);
        free(params_.updateCharacter.progresses);
        free(params_.updateCharacter.quests);
        session_shutdown(session);
    }

    params_.updateCharacter.keyMapEntryCount = 0;
    for (uint8_t i = 0; i < KEYMAP_MAX_KEYS; i++) {
        if (chr->keyMap[i].type != 0) {
            params_.updateCharacter.keyMap[params_.updateCharacter.keyMapEntryCount].key = i;
            params_.updateCharacter.keyMap[params_.updateCharacter.keyMapEntryCount].type = chr->keyMap[i].type;
            params_.updateCharacter.keyMap[params_.updateCharacter.keyMapEntryCount].action = chr->keyMap[i].action;
            params_.updateCharacter.keyMapEntryCount++;
        }
    }

    actor->req = database_request_create(thread_ctx->conn, &params_);
    if (actor->req == NULL) {
        database_connection_unlock(thread_ctx->conn);
        free(params_.updateCharacter.monsterBook);
        free(params_.updateCharacter.skills);
        free(params_.updateCharacter.completedQuests);
        free(params_.updateCharacter.questInfos);
        free(params_.updateCharacter.progresses);
        free(params_.updateCharacter.quests);
        session_shutdown(session);
    }

    continue_flush_character(worker, 0, user_data);
}

static void continue_flush_character(struct Worker *worker, int status, void *user_data)
{
    struct ThreadContext *thread_ctx = worker_get_user_data(worker);
    struct Actor *actor = user_data;

    status = database_request_execute(actor->req, status);
    if (status <= 0) {
        const struct RequestParams *params = database_request_get_params(actor->req);
        free(params->updateCharacter.keyMap);
        free(params->updateCharacter.monsterBook);
        free(params->updateCharacter.skills);
        free(params->updateCharacter.completedQuests);
        free(params->updateCharacter.questInfos);
        free(params->updateCharacter.progresses);
        free(params->updateCharacter.quests);
        database_request_destroy(actor->req);
        database_connection_unlock(thread_ctx->conn);
        if (status == 0) {
            //io_worker_post(save->server->io, notify_login_server, save);
        }
    } else {
        worker_poll(worker, database_connection_get_fd(thread_ctx->conn), status, continue_flush_character, actor);
    }

    // TODO: Notify login server
}

static void add_quest(void *data, void *ctx_)
{
    struct Quest *quest = data;
    struct {
        uint16_t *quests;
        size_t currentQuest;
        size_t progressCount;
    } *ctx = ctx_;

    ctx->quests[ctx->currentQuest] = quest->id;
    ctx->progressCount += quest->progressCount;
    ctx->currentQuest++;
}

static void add_progress(void *data, void *ctx_)
{
    struct Quest *quest = data;
    struct {
        struct DatabaseProgress *progresses;
        size_t currentProgress;
    } *ctx = ctx_;
    const struct QuestInfo *info = wz_get_quest_info(quest->id);

    const struct QuestRequirement *req;
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_MOB) {
            req = &info->endRequirements[i];
            break;
        }
    }

    for (uint8_t i = 0; i < quest->progressCount; i++) {
        ctx->progresses[ctx->currentProgress].questId = quest->id;
        ctx->progresses[ctx->currentProgress].progressId = req->mob.mobs[i].id;
        ctx->progresses[ctx->currentProgress].progress = quest->progress[i];
        ctx->currentProgress++;
    }
}

static void add_quest_info(void *data, void *ctx_)
{
    struct QuestInfoProgress *info = data;
    struct {
        struct DatabaseInfoProgress *infos;
        size_t currentInfo;
    } *ctx = ctx_;

    ctx->infos[ctx->currentInfo].infoId = info->id;
    ctx->infos[ctx->currentInfo].progressLength = info->length;
    strncpy(ctx->infos[ctx->currentInfo].progress, info->value, info->length);
    ctx->currentInfo++;
}

static void add_completed_quest(void *data, void *ctx_)
{
    struct CompletedQuest *quest = data;
    struct {
        struct DatabaseCompletedQuest *quests;
        size_t currentQuest;
    } *ctx = ctx_;

    ctx->quests[ctx->currentQuest].id = quest->id;
    struct tm tm;
    gmtime_r(&quest->time, &tm);
    ctx->quests[ctx->currentQuest].time.year = tm.tm_year + 1900;
    ctx->quests[ctx->currentQuest].time.month = tm.tm_mon + 1;
    ctx->quests[ctx->currentQuest].time.day = tm.tm_mday;
    ctx->quests[ctx->currentQuest].time.hour = tm.tm_hour;
    ctx->quests[ctx->currentQuest].time.minute = tm.tm_min;
    ctx->quests[ctx->currentQuest].time.second = tm.tm_sec;
    ctx->quests[ctx->currentQuest].time.second_part = 0;
    ctx->quests[ctx->currentQuest].time.neg = 0;
    ctx->currentQuest++;
}

static void add_skill(void *data, void *ctx_)
{
    struct Skill *skill = data;
    struct {
        struct DatabaseSkill *skills;
        size_t currentSkill;
    } *ctx = ctx_;

    ctx->skills[ctx->currentSkill].id = skill->id;
    ctx->skills[ctx->currentSkill].level = skill->level;
    ctx->skills[ctx->currentSkill].masterLevel = skill->masterLevel;
    ctx->currentSkill++;
}

static void add_monster_book_entry(void *data, void *ctx_)
{
    struct MonsterBookEntry *entry = data;
    struct {
        struct DatabaseMonsterBookEntry *monsterBook;
        size_t currentEntry;
    } *ctx = ctx_;

    ctx->monsterBook[ctx->currentEntry].id = entry->id;
    ctx->monsterBook[ctx->currentEntry].quantity = entry->count;
    ctx->currentEntry++;
}


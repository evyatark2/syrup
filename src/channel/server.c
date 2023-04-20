#include "server.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <fcntl.h>
#include <limits.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <sys/un.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include "../crypt.h"
#include "../hash-map.h"

#define MAPLE_VERSION 83

enum SessionState {
    SESSION_STATE_CONNECTING,
    SESSION_STATE_CONNECTED,
    SESSION_STATE_KICKING,
    SESSION_STATE_DISCONNECTING,
};

struct Session {
    struct sockaddr_storage addr;
    //struct sockaddr_storage addr;
    void *supervisor;
    uint32_t token;
    struct Room *room;
    /// The current state of the client
    enum SessionState state;
    /// The number of jobs this client is currently pending on
    struct bufferevent *event;
    struct EncryptionContext *sendContext;
    struct DecryptionContext *recieveContext;
    struct event *userEvent;
    OnResume *onResume;
    bool writeEnable;
    uint32_t targetRoom;
    void *userData;
};

struct RoomThread {
    uint32_t room;
    size_t thread;
};

struct LoggedOutNode {
    struct LoggedOutNode *next;
    uint32_t token;
};

struct Worker {
    OnLog *onLog;
    OnClientDisconnect *onClientDisconnect;
    OnClientPacket *onClientPacket;
    DestroyUserContext *destroyContext;

    /// Used to lock writes to the sinks
    mtx_t *transportMuteces;
    /// Transport pipes used to transfer connected client to an available worker
    int *transportSinks;

    struct event_base *base;
    union {
        struct event *transportEvent;
        struct evconnlistener *listener;
    };
    void *userData;
    struct event *userEvent;

    mtx_t *roomMapLock;
    struct HashSetU32 *roomMap; // Uses `struct RoomThread`

    mtx_t *sessionsLock;
    struct HashSetAddr *sessions; // Uses `struct SessionRoom`

    mtx_t *mtx;
    bool *connected;
    struct bufferevent **login;
    struct LoggedOutNode **head;
};

struct HeapNode {
    size_t threadIndex;
    size_t sessionCount;
};

struct PendingCharacter {
    uint32_t token;
    uint32_t id;
};

struct AddrSession {
    struct sockaddr_storage addr;
    struct Session *session;
};

struct ChannelServer {
    struct Worker worker;
    OnClientConnect *onClientConnect;

    /// Worker thread handles
    size_t threadCount;
    thrd_t *threads;

    /// Minimum heap used to find the least busy worker
    mtx_t heapLock;
    struct HeapNode *minHeap;

    // Sessions that still don't have a room assined, uses `struct AddrSession`
    struct HashSetAddr *sessions;

    int commandSink;
    struct event *commandEvent;

    void *userData;

    struct evconnlistener *loginListener;
    bool isUnix;
    struct sockaddr_storage addr;
    int socklen;
    uint8_t first;

    struct HashSetU32 *pendings;
};

static void on_command(int fd, short what, void *ctx);
static void on_session_connect(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *, int socklen, void *ctx);
static void on_login_server_connect(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *, int socklen, void *ctx);
static void on_timer_expired(int fd, short what, void *ctx);

struct TimerEvent;

struct TimerHandle {
    struct Room *room;
    size_t index;
    void *data;
    struct TimerEvent *node;
    // Whether this timer should continue running even if all players leave the map
    // Use 'false' for something like mob respawn timer
    // Use 'true' for something like a drop timer
    bool keepAlive;
};

struct Room {
    uint32_t id;
    struct RoomManager *manager;
    struct HashSetAddr *sessions; // Uses struct AddrSession
    size_t timerCapacity;
    size_t timerCount;
    struct TimerHandle **timers;
    void *userData;
};

static struct Room *create_room(struct RoomManager *manager, uint32_t id);

struct RoomId {
    uint32_t id;
    struct Room *room;
};

struct TimerEvent {
    struct timespec time;
    void (*f)(struct Room *room, struct TimerHandle *handle);
    struct TimerHandle *handle;
};

struct TimerEventHeap {
    size_t capacity;
    size_t count;
    struct TimerEvent *events;
};

static int event_heap_init(struct TimerEventHeap *heap);
static void event_heap_destroy(struct TimerEventHeap *heap);
static int event_heap_push(struct TimerEventHeap *heap, struct timespec tp, void (*f)(struct Room *, struct TimerHandle *), struct TimerHandle *handle);
static struct TimerEvent *event_heap_top(struct TimerEventHeap *heap);
static struct TimerEvent event_heap_removetop(struct TimerEventHeap *heap);
static bool event_heap_remove(struct TimerEventHeap *heap, struct TimerEvent *node);

struct SessionRoom {
    struct sockaddr_storage addr;
    uint32_t room;
};

// TODO: Maybe use a thread_local global instead of passing the struct between event callbacks
struct RoomManager {
    struct Worker worker;
    OnClientJoin *onClientJoin;
    OnResume *onResumeClientJoin;
    OnRoomCreate *onRoomCreate;
    OnRoomDestroy *onRoomDestroy;
    struct HashSetAddr *sessions; // Uses `struct SessionRoom`
    struct HashSetU32 *rooms; // Uses `RoomId`
    struct event *timer;
    struct TimerEventHeap heap;
    void *userData;
};

static int start_worker(void *ctx_);

static void on_session_join(int fd, short what, void *ctx_);
static void on_greeter_user_fd_ready(int fd, short what, void *ctx);
static void on_worker_user_fd_ready(int fd, short what, void *ctx);

static struct Session *create_session(struct ChannelServer *server, int fd, struct sockaddr *addr, int socklen);
static int finalize_session_connect(struct Session *session);
static void destroy_pending_session(struct Session *session, bool async);
static void destroy_session(struct Session *session, bool async);
static bool kick_pending_session(struct Session *session, bool async);
static bool kick_session(struct Session *session, bool async);
static void destroy_room(struct RoomManager *manager, struct Room *room, bool async);

static short poll_to_libevent(int mask);
static int libevent_to_poll(short mask);

static void do_pending_assign(struct Session *session);
static void do_transfer(struct Session *session);

struct ChannelServer *channel_server_create(uint16_t port, OnLog *on_log, const char *host, CreateUserContext *create_user_context, DestroyUserContext destroy_user_ctx, OnClientConnect *on_client_connect, OnClientDisconnect *on_client_disconnect, OnClientJoin *on_client_join, OnClientPacket *on_pending_client_packet, OnClientPacket *on_client_packet, OnRoomCreate *on_room_create, OnRoomDestroy *on_room_destroy, void *global_ctx)
{
    struct ChannelServer *server = malloc(sizeof(struct ChannelServer));
    if (server == NULL)
        return NULL;

    evthread_use_pthreads();

    server->worker.base = event_base_new();
    if (server->worker.base == NULL)
        goto free_server;

    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    if (evutil_inet_pton(AF_INET, host, &addr4) == 1) {
        server->isUnix = false;
        memcpy(&server->addr, &addr4, sizeof(struct sockaddr_in));
        server->socklen = sizeof(struct sockaddr_in);
    } else if (evutil_inet_pton(AF_INET6, host, &addr6) == 1) {
        server->isUnix = false;
        memcpy(&server->addr, &addr6, sizeof(struct sockaddr_in6));
        server->socklen = sizeof(struct sockaddr_in6);
    } else {
        server->isUnix = true;
        struct sockaddr_un addr = {
            .sun_family = AF_UNIX,
        };
        strcpy(addr.sun_path, host);
        memcpy(&server->addr, &addr, sizeof(struct sockaddr_un));
        server->socklen = sizeof(struct sockaddr_un);
    }

    server->loginListener = evconnlistener_new_bind(server->worker.base, on_login_server_connect, server, LEV_OPT_CLOSE_ON_FREE, 1, (void *)&server->addr, server->socklen);

    server->pendings = hash_set_u32_create(sizeof(struct PendingCharacter), offsetof(struct PendingCharacter, token));

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port),
    };

    server->worker.listener = evconnlistener_new_bind(server->worker.base, on_session_connect, server, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, (const struct sockaddr *)&addr, sizeof(struct sockaddr_in));
    if (server->worker.listener == NULL)
        goto free_base;

    server->worker.roomMap = hash_set_u32_create(sizeof(struct RoomThread), offsetof(struct RoomThread, room));
    server->worker.roomMapLock = malloc(sizeof(mtx_t));
    mtx_init(server->worker.roomMapLock, mtx_plain);

    server->worker.sessions = hash_set_addr_create(sizeof(struct SessionRoom), offsetof(struct SessionRoom, addr));
    server->worker.sessionsLock = malloc(sizeof(mtx_t));
    mtx_init(server->worker.sessionsLock, mtx_plain);

    server->worker.mtx = malloc(sizeof(mtx_t));
    mtx_init(server->worker.mtx, mtx_plain);

    server->sessions = hash_set_addr_create(sizeof(struct AddrSession), offsetof(struct AddrSession, addr));

    server->worker.destroyContext = destroy_user_ctx;
    server->worker.onClientDisconnect = on_client_disconnect;
    server->worker.onClientPacket = on_pending_client_packet;
    server->onClientConnect = on_client_connect;
    server->worker.onLog = on_log;
    server->worker.userData = create_user_context();
    server->worker.connected = malloc(sizeof(bool));
    *server->worker.connected = false;
    server->worker.login = malloc(sizeof(struct bufferevent *));
    *server->worker.login = NULL;
    server->worker.head = malloc(sizeof(struct LoggedOutNode *));
    *server->worker.head = NULL;

    long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    server->worker.transportMuteces = malloc(nproc * sizeof(mtx_t));
    if (server->worker.transportMuteces == NULL)
        goto free_listener;

    // Start the worker threads
    server->worker.transportSinks = malloc(nproc * sizeof(int));
    if (server->worker.transportSinks == NULL)
        goto free_muteces;

    int pipefds[2];
    if (pipe(pipefds) == -1)
        goto free_sinks;

    server->commandSink = pipefds[1];
    server->commandEvent = event_new(server->worker.base, pipefds[0], EV_READ | EV_PERSIST, on_command, server);
    if (server->commandEvent == NULL) {
        close(pipefds[0]);
        goto close_command;
    }

    if (event_add(server->commandEvent, NULL) == -1)
        goto free_command_event;

    server->threads = malloc(nproc * sizeof(thrd_t));
    if (server->threads == NULL)
        goto free_command_event;

    for (server->threadCount = 0; server->threadCount < nproc; server->threadCount++) {
        struct RoomManager *manager;
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1)
            goto exit_threads;

        if (mtx_init(&server->worker.transportMuteces[server->threadCount], mtx_plain) != thrd_success) {
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        server->worker.transportSinks[server->threadCount] = pair[1];

        manager = malloc(sizeof(struct RoomManager));
        if (manager == NULL) {
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        manager->worker.userData = create_user_context();
        if (manager->worker.userData == NULL) {
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        manager->worker.base = event_base_new();
        if (manager->worker.base == NULL) {
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        manager->worker.transportEvent = event_new(manager->worker.base, pair[0], EV_READ | EV_PERSIST, on_session_join, manager);
        if (manager->worker.transportEvent == NULL) {
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        if (event_add(manager->worker.transportEvent, NULL) == -1) {
            event_free(manager->worker.transportEvent);
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        manager->rooms = hash_set_u32_create(sizeof(struct RoomId), offsetof(struct RoomId, id));
        if (manager->rooms == NULL) {
            event_free(manager->worker.transportEvent);
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        manager->sessions = hash_set_addr_create(sizeof(struct SessionRoom), offsetof(struct SessionRoom, addr));
        if (manager->sessions == NULL) {
            hash_set_u32_destroy(manager->rooms);
            event_free(manager->worker.transportEvent);
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }


        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (fd == -1) {
            hash_set_addr_destroy(manager->sessions);
            hash_set_u32_destroy(manager->rooms);
            event_free(manager->worker.transportEvent);
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }
        manager->timer = event_new(manager->worker.base, fd, EV_READ | EV_PERSIST, on_timer_expired, manager);
        if (manager->timer == NULL) {
            close(fd);
            hash_set_addr_destroy(manager->sessions);
            hash_set_u32_destroy(manager->rooms);
            event_free(manager->worker.transportEvent);
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }
        event_add(manager->timer, NULL);

        if (event_heap_init(&manager->heap) == -1) {
            event_free(manager->timer);
            close(fd);
            hash_set_addr_destroy(manager->sessions);
            hash_set_u32_destroy(manager->rooms);
            event_free(manager->worker.transportEvent);
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }

        manager->worker.onLog = on_log;
        manager->worker.onClientDisconnect = on_client_disconnect;
        manager->worker.onClientPacket = on_client_packet;
        manager->worker.transportMuteces = server->worker.transportMuteces;
        manager->worker.transportSinks = server->worker.transportSinks;
        manager->worker.destroyContext = destroy_user_ctx;
        manager->worker.roomMapLock = server->worker.roomMapLock;
        manager->worker.roomMap = server->worker.roomMap;
        manager->worker.sessionsLock = server->worker.sessionsLock;
        manager->worker.sessions = server->worker.sessions;
        manager->worker.mtx = server->worker.mtx;
        manager->worker.login = server->worker.login;
        manager->worker.connected = server->worker.connected;
        manager->worker.head = server->worker.head;
        manager->onClientJoin = on_client_join;
        manager->onRoomCreate = on_room_create;
        manager->onRoomDestroy = on_room_destroy;
        manager->userData = global_ctx;
        if (thrd_create(server->threads + server->threadCount, start_worker, manager) != thrd_success) {
            event_heap_destroy(&manager->heap);
            event_free(manager->timer);
            close(fd);
            hash_set_addr_destroy(manager->sessions);
            hash_set_u32_destroy(manager->rooms);
            event_free(manager->worker.transportEvent);
            event_base_free(manager->worker.base);
            destroy_user_ctx(manager->worker.userData);
            free(manager);
            mtx_destroy(&server->worker.transportMuteces[server->threadCount]);
            close(pair[0]);
            close(pair[1]);
            goto exit_threads;
        }
    }

    server->userData = global_ctx;
    server->first = 0;

    return server;


exit_threads:
    for (size_t i = 0; i < server->threadCount; i++) {
        mtx_destroy(&server->worker.transportMuteces[i]);
        close(server->worker.transportSinks[i]);
        thrd_join(server->threads[i], NULL);
    }

    free(server->threads);

free_command_event:
    close(event_get_fd(server->commandEvent));
    event_free(server->commandEvent);

close_command:
    close(server->commandSink);

free_sinks:
    free(server->worker.transportSinks);

free_muteces:
    free(server->worker.transportMuteces);

free_listener:
    evconnlistener_free(server->worker.listener);

free_base:
    event_base_free(server->worker.base);

free_server:
    free(server);
    return NULL;
}

void channel_server_destroy(struct ChannelServer *server)
{
    for (size_t i = 0; i < server->threadCount; i++)
        mtx_destroy(&server->worker.transportMuteces[i]);

    mtx_destroy(server->worker.mtx);
    free(server->worker.mtx);
    while (*server->worker.head != NULL) {
        struct LoggedOutNode *next = (*server->worker.head)->next;
        free(*server->worker.head);
        *server->worker.head = next;
    }

    free(server->worker.head);
    free(server->worker.connected);
    free(server->worker.login);

    hash_set_u32_destroy(server->pendings);
    free(server->threads);
    mtx_destroy(server->worker.sessionsLock);
    free(server->worker.sessionsLock);
    mtx_destroy(server->worker.roomMapLock);
    free(server->worker.roomMapLock);
    hash_set_u32_destroy(server->worker.roomMap);
    hash_set_addr_destroy(server->sessions);
    hash_set_addr_destroy(server->worker.sessions);
    event_base_free(server->worker.base);
    server->worker.destroyContext(server->worker.userData);
    free(server->worker.transportMuteces);
    free(server->worker.transportSinks);
    free(server);
}

enum ResponderResult channel_server_start(struct ChannelServer *server)
{
    int status = event_base_dispatch(server->worker.base);

    for (size_t i = 0; i < server->threadCount; i++) {
        mtx_lock(&server->worker.transportMuteces[i]);
        close(server->worker.transportSinks[i]);
        server->worker.transportSinks[i] = -1;
        mtx_unlock(&server->worker.transportMuteces[i]);
    }

    for (size_t i = 0; i < server->threadCount; i++)
        thrd_join(server->threads[i], NULL);

    return status == -1 ? RESPONDER_RESULT_ERROR : RESPONDER_RESULT_SUCCESS;
}

void channel_server_stop(struct ChannelServer *server)
{
    close(server->commandSink);
}

bool session_assign_token(struct Session *session, uint32_t token, uint32_t *id)
{
    struct ChannelServer *server = session->supervisor;
    struct PendingCharacter *pending = hash_set_u32_get(server->pendings, token);
    if (pending == NULL)
        return false;

    *id = pending->id;
    hash_set_u32_remove(server->pendings, token);
    session->token = token;
    return true;
}

void session_write(struct Session *session, size_t len, uint8_t *packet)
{
    if (len == 0)
        return;

    uint16_t packet_len = len;
    if (bufferevent_write(session->event, &packet_len, 2) == -1)
        return;

    bufferevent_write(session->event, packet, len);
}

void session_set_context(struct Session *session, void *ctx)
{
    session->userData = ctx;
}

void *session_get_context(struct Session *session)
{
    return session->userData;
}

int session_get_event_disposition(struct Session *session)
{
    return libevent_to_poll(event_get_events(session->userEvent));
}

int session_get_event_fd(struct Session *session)
{
    return session->userEvent != NULL ? event_get_fd(session->userEvent) : -1;
}

int session_set_event(struct Session *session, int status, int fd, OnResume *on_resume)
{
    struct Worker *worker = session->supervisor;
    if (session->userEvent != NULL)
        event_free(session->userEvent);

    session->userEvent = event_new(worker->base, fd, poll_to_libevent(status) | EV_PERSIST, session->targetRoom != -1 ? on_worker_user_fd_ready : on_greeter_user_fd_ready, session);
    if (session->userEvent == NULL)
        return -1;

    if (event_add(session->userEvent, NULL) == -1) {
        event_free(session->userEvent);
        session->userEvent = NULL;
        return -1;
    }

    session->onResume = on_resume;
    return 0;
}

void session_close_event(struct Session *session)
{
    int fd = event_get_fd(session->userEvent);
    event_free(session->userEvent);
    session->userEvent = NULL;
    close(fd);
}

struct Room *session_get_room(struct Session *session)
{
    return session->room;
}

enum WorkerCommandType {
    WORKER_COMMAND_NEW_CLIENT,
    WORKER_COMMAND_KICK,
};

struct WorkerCommand {
    enum WorkerCommandType type;
    union {
        struct {
            int fd;
            struct Session *session;
        };
        struct {
            struct sockaddr_storage addr;
            int socklen;
        };
    };
};

struct BroadcastContext {
    struct Session *session;
    size_t len;
    uint8_t *packet;
};

static void do_broadcast(void *data, void *ctx_)
{
    struct Session *current = ((struct AddrSession *)data)->session;
    struct BroadcastContext *ctx = ctx_;
    if (current != ctx->session && current->writeEnable)
        session_write(current, ctx->len, ctx->packet);
}

void session_broadcast_to_room(struct Session *session, size_t len, uint8_t *packet)
{
    struct BroadcastContext ctx = {
        .session = session,
        .len = len,
        .packet = packet
    };
    hash_set_addr_foreach(session->room->sessions, do_broadcast, &ctx);
}

struct ForeachContext {
    struct Session *session;
    void (*f)(struct Session *src, struct Session *dst, void *ctx);
    void *ctx;
};

static void do_foreach(void *data, void *ctx_)
{
    struct Session *current = ((struct AddrSession *)data)->session;
    struct ForeachContext *ctx = ctx_;
    if (current != ctx->session)
        ctx->f(ctx->session, current, ctx->ctx);
}

void session_foreach_in_room(struct Session *session, void (*f)(struct Session *src, struct Session *dst, void *ctx), void *ctx_)
{
    struct ForeachContext ctx = {
        .session = session,
        .f = f,
        .ctx = ctx_
    };
    hash_set_addr_foreach(session->room->sessions, do_foreach, &ctx);
}

void session_enable_write(struct Session *session)
{
    session->writeEnable = true;
}

uint32_t room_get_id(struct Room *room)
{
    return room->id;
}

void room_set_context(struct Room *room, void *ctx)
{
    room->userData = ctx;
}

void *room_get_context(struct Room *room)
{
    return room->userData;
}

void room_broadcast(struct Room *room, size_t len, uint8_t *packet)
{
    struct BroadcastContext ctx = {
        .session = NULL,
        .len = len,
        .packet = packet
    };
    hash_set_addr_foreach(room->sessions, do_broadcast, &ctx);
}

void room_foreach(struct Room *room, void (*f)(struct Session *src, struct Session *dst, void *ctx), void *ctx_)
{
    struct ForeachContext ctx = {
        .session = NULL,
        .f = f,
        .ctx = ctx_
    };
    hash_set_addr_foreach(room->sessions, do_foreach, &ctx);
}

struct TimerHandle *room_add_timer(struct Room *room, uint64_t msec, void (*f)(struct Room *, struct TimerHandle *), void *data, bool keep_alive)
{
    struct RoomManager *manager = room->manager;
    struct TimerHandle *handle = malloc(sizeof(struct TimerHandle));
    if (handle == NULL)
        return NULL;

    if (room->timerCount == room->timerCapacity) {
        void *temp = realloc(room->timers, (room->timerCapacity * 2) * sizeof(struct TimerHandle *));
        if (temp == NULL) {
            free(handle);
            return NULL;
        }

        room->timers = temp;
        room->timerCapacity *= 2;
    }

    handle->room = room;
    handle->index = room->timerCount;
    handle->data = data;
    handle->keepAlive = keep_alive;

    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    tp.tv_nsec += (msec % 1000) * 1000000;
    if (tp.tv_nsec >= 1000000000) {
        tp.tv_nsec -= 1000000000;
        tp.tv_sec++;
    }
    tp.tv_sec += msec / 1000;
    if (event_heap_push(&manager->heap, tp, f, handle) == -1) {
        free(handle);
        return NULL;
    }

    if (event_heap_top(&manager->heap) == handle->node) {
        struct itimerspec time = {
            .it_value = tp,
            .it_interval = { 0 },
        };
        timerfd_settime(event_get_fd(manager->timer), TFD_TIMER_ABSTIME, &time, NULL); // timerfd_settime() can only fail due to user error which shouldn't be the case here
    }

    room->timers[room->timerCount] = handle;
    room->timerCount++;

    return handle;
}

struct Room *timer_get_room(struct TimerHandle *handle)
{
    return handle->room;
}

void timer_set_data(struct TimerHandle *handle, void *data)
{
    handle->data = data;
}

void *timer_get_data(struct TimerHandle *handle)
{
    return handle->data;
}

void room_stop_timer(struct TimerHandle *handle)
{
    struct Room *room = handle->room;
    struct RoomManager *manager = room->manager;
    room->timers[handle->index] = room->timers[room->timerCount - 1];
    room->timers[handle->index]->index = handle->index;
    room->timerCount--;

    if (event_heap_remove(&manager->heap, handle->node)) {
        struct TimerEvent *next = event_heap_top(&manager->heap);
        if (next != NULL) {
            struct itimerspec spec = { .it_value = next->time, .it_interval = { 0, 0 } };
            timerfd_settime(event_get_fd(manager->timer), TFD_TIMER_ABSTIME, &spec, NULL);
        } else {
            // This was the last timer; Disarm the timer
            struct itimerspec spec = { .it_value = { 0, 0 } };
            timerfd_settime(event_get_fd(manager->timer), 0, &spec, NULL);
        }
    }

    free(handle);
}

struct RoomContext {
    struct RoomList *list;
    size_t index;
};

enum CommandType {
    COMMAND_TYPE_KICK
};

struct Command {
    enum CommandType type;
};

static bool do_pending_kick(void *data, void *ctx)
{
    return kick_pending_session(((struct AddrSession *)data)->session, true);
}

static void on_command(int fd, short what, void *ctx)
{
    struct ChannelServer *server = ctx;
    struct Command command;
    ssize_t status = read(fd, &command, sizeof(struct Command));
    if (status == 0) {
        mtx_lock(server->worker.mtx);
        *server->worker.connected = false;
        mtx_unlock(server->worker.mtx);

        if (*server->worker.login != NULL) {
            bufferevent_free(*server->worker.login);
        } else {
            evconnlistener_free(server->loginListener);
            if (server->isUnix)
                unlink(((struct sockaddr_un *)&server->addr)->sun_path);
        }

        close(event_get_fd(server->commandEvent));
        event_free(server->commandEvent);
        evconnlistener_free(server->worker.listener);
        server->worker.listener = NULL;
        hash_set_addr_foreach_with_remove(server->sessions, do_pending_kick, NULL);
    } else if (status == -1) {
    } else {
    }
}

static void on_session_connect(struct evconnlistener *listener, evutil_socket_t fd,
        struct sockaddr *addr, int socklen, void *ctx_)
{
    struct ChannelServer *server = ctx_;

    struct Session *session = create_session(server, fd, addr, socklen);
    if (session == NULL) {
        close(fd);
        return;
    }

    int status = server->onClientConnect(session, server->userData, server->worker.userData, addr);
    if (status == 0) {
        if (session->userEvent == NULL && finalize_session_connect(session) == -1)
            kick_pending_session(session, false);
    } else {
        bufferevent_free(session->event);
        close(fd);
        encryption_context_destroy(session->sendContext);
        decryption_context_destroy(session->recieveContext);
        hash_set_addr_remove(server->sessions, addr);
    }
}

static void on_login_server_read(struct bufferevent *bev, void *ctx);
static void on_login_server_event(struct bufferevent *bev, short what, void *ctx);

static void on_login_server_connect(struct evconnlistener *listener, evutil_socket_t fd,
        struct sockaddr *addr, int socklen, void *ctx)
{
    struct ChannelServer *server = ctx;
    // TODO: Check if addr is allowed
    *server->worker.login = bufferevent_socket_new(evconnlistener_get_base(listener), fd,
            BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    if (*server->worker.login == NULL) {
        close(fd);
        return;
    }

    evconnlistener_free(listener);
    if (server->isUnix)
        unlink(((struct sockaddr_un *)&server->addr)->sun_path);

    bufferevent_setcb(*server->worker.login, on_login_server_read, NULL, on_login_server_event, ctx);
    bufferevent_enable(*server->worker.login, EV_READ | EV_WRITE);

    bufferevent_write(*server->worker.login, &server->first, 1);
    if (server->first == 0) {
        server->first = 1;
        mtx_lock(server->worker.mtx);
        *server->worker.connected = true;
        mtx_unlock(server->worker.mtx);
    }
}

static void do_send_kick_command(void *data, void *ctx)
{
    struct SessionRoom *pair = data;
    struct ChannelServer *server = ctx;
    size_t thread = -1;
    mtx_lock(server->worker.roomMapLock);
    if (hash_set_u32_get(server->worker.roomMap, pair->room) != NULL)
        thread = ((struct RoomThread *)hash_set_u32_get(server->worker.roomMap, pair->room))->thread;
    mtx_unlock(server->worker.roomMapLock);

    if (thread != -1) {
        struct WorkerCommand command = {
            .type = WORKER_COMMAND_KICK,
            .addr = pair->addr,
        };

        mtx_lock(&server->worker.transportMuteces[thread]);
        write(server->worker.transportSinks[thread], &command, sizeof(struct WorkerCommand));
        mtx_unlock(&server->worker.transportMuteces[thread]);
    }
}

static void on_login_server_read(struct bufferevent *bev, void *ctx)
{
    struct ChannelServer *server = ctx;

    if (!*server->worker.connected) {
        uint8_t reset;
        evbuffer_remove(bufferevent_get_input(bev), &reset, 1);
        if (reset == 1) {
            mtx_lock(server->worker.sessionsLock);
            hash_set_addr_foreach(server->worker.sessions, do_send_kick_command, server);
            mtx_unlock(server->worker.sessionsLock);

            mtx_lock(server->worker.mtx);
            *server->worker.connected = true;
            mtx_unlock(server->worker.mtx);
        } else {
            mtx_lock(server->worker.mtx);
            while (*server->worker.head != NULL) {
                uint8_t data[5];
                data[0] = 1;
                memcpy(data + 1, &(*server->worker.head)->token, 4);
                bufferevent_write(*server->worker.login, data, 5);
                struct LoggedOutNode *next = (*server->worker.head)->next;
                free(*server->worker.head);
                *server->worker.head = next;
            }
            *server->worker.connected = true;
            mtx_unlock(server->worker.mtx);
        }
    } else {
        while (evbuffer_get_length(bufferevent_get_input(bev)) >= 8) {
            // TODO: Time the pending character
            struct PendingCharacter pending;
            evbuffer_remove(bufferevent_get_input(bev), &pending.token, sizeof(uint32_t));
            evbuffer_remove(bufferevent_get_input(bev), &pending.id, sizeof(uint32_t));

            hash_set_u32_insert(server->pendings, &pending);

            uint8_t data[5];
            data[0] = 0;
            memcpy(data + 1, &pending.token, sizeof(uint32_t));
            bufferevent_write(bev, data, 5);
        }
    }
}

static void on_login_server_event(struct bufferevent *bev, short what, void *ctx)
{
    struct ChannelServer *server = ctx;
    server->loginListener = evconnlistener_new_bind(bufferevent_get_base(bev), on_login_server_connect, ctx, LEV_OPT_CLOSE_ON_FREE, 1, (void *)&server->addr, server->socklen);
    mtx_lock(server->worker.mtx);
    *server->worker.connected = false;
    mtx_unlock(server->worker.mtx);
    bufferevent_free(bev);
    *server->worker.login = NULL;
}

static long timespec_cmp(struct timespec *tp1, struct timespec *tp2);

static void on_timer_expired(int fd, short what, void *ctx)
{
    struct RoomManager *manager = ctx;
    uint64_t count;
    if (read(fd, &count, 8) == -1)
        return; // EAGAIN can happen if the timer has expired and at the same time we removed the top event in the heap in some previous callback
    struct TimerEvent *next;
    struct timespec current_time;
    do {
        struct TimerEvent ev = event_heap_removetop(&manager->heap);
        struct Room *room = ev.handle->room;
        ev.f(room, ev.handle);
        room->timers[ev.handle->index] = room->timers[room->timerCount - 1];
        room->timers[ev.handle->index]->index = ev.handle->index;
        room->timerCount--;
        if (room->timerCount == 0 && hash_set_addr_size(room->sessions) == 0) {
            manager->onRoomDestroy(ev.handle->room);
            free(room->timers);
            hash_set_u32_remove(manager->rooms, room->id);

            hash_set_addr_destroy(room->sessions);

            mtx_lock(manager->worker.roomMapLock);
            hash_set_u32_remove(manager->worker.roomMap, room->id);
            mtx_unlock(manager->worker.roomMapLock);

            free(room);
        }
        free(ev.handle);
        next = event_heap_top(&manager->heap);
        assert(clock_gettime(CLOCK_MONOTONIC, &current_time) != -1);
    } while (next != NULL && timespec_cmp(&next->time, &current_time) <= 0);

    if (next != NULL) {
        struct itimerspec time = {
            .it_value = next->time
        };

        assert(timerfd_settime(fd, TFD_TIMER_ABSTIME, &time, NULL) != -1);
    }
}

static enum bufferevent_filter_result input_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx);
static enum bufferevent_filter_result output_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx);
static void on_session_read(struct bufferevent *event, void *ctx_);
static void on_pending_session_read(struct bufferevent *event, void *ctx_);
static void on_session_write(struct bufferevent *event, void *ctx_);
static void on_pending_session_write(struct bufferevent *event, void *ctx_);

static void on_session_event(struct bufferevent *event, short what, void *ctx);
static void on_pending_session_event(struct bufferevent *event, short what, void *ctx);

static void do_kill_timers(void *data, void *ctx)
{
    struct Room *room = ((struct RoomId *)data)->room;
    struct RoomManager *manager = ctx;
    for (size_t i = 0; i < room->timerCount; i++) {
        struct TimerHandle *timer = room->timers[i];
        if (timer->keepAlive) {
            event_heap_remove(&manager->heap, timer->node);
            room->timers[i] = room->timers[room->timerCount - 1];
            room->timers[i]->index = i;
            free(timer);
            room->timerCount--;
            i--;
        }
    }
}

static bool do_kick(void *data, void *ctx)
{
    return kick_session(((struct AddrSession *)data)->session, true);
}

static bool do_kill_room(void *data, void *ctx)
{
    struct Room *room = ((struct RoomId *)data)->room;
    struct RoomManager *manager = ctx;

    hash_set_addr_foreach_with_remove(room->sessions, do_kick, room);

    if (hash_set_addr_size(room->sessions) == 0) {
        destroy_room(manager, room, true);
        return true;
    }

    return false;
}

static void on_session_join(int fd, short what, void *ctx_)
{
    struct RoomManager *manager = ctx_;
    ssize_t status;
    struct WorkerCommand command;

    if ((status = read(fd, &command, sizeof(struct WorkerCommand))) == 0) {
        // Shutdown request
        int fd = event_get_fd(manager->worker.transportEvent);
        event_free(manager->worker.transportEvent);
        close(fd);
        // Start by killing timers that are marked 'keepAlive'
        hash_set_u32_foreach(manager->rooms, do_kill_timers, manager);

        event_del(manager->timer);

        // Timers that aren't keepAlive will be killed here as part of destroy_room()
        hash_set_u32_foreach_with_remove(manager->rooms, do_kill_room, manager);
    } else if (status != -1) {
        switch (command.type) {
        case WORKER_COMMAND_NEW_CLIENT: {
            struct Session *session = command.session;
            if (hash_set_u32_get(manager->rooms, session->targetRoom) == NULL) {
                session->room = create_room(manager, session->targetRoom);
                if (command.session->room == NULL)
                    ; // TODO
                struct RoomId new = {
                    .id = session->targetRoom,
                    .room = session->room
                };
                if (hash_set_u32_insert(manager->rooms, &new) == -1)
                    ; // TODO
                if (manager->onRoomCreate(session->room, manager->userData) != 0)
                    ; // TODO
            } else {
                session->room = ((struct RoomId *)hash_set_u32_get(manager->rooms, session->targetRoom))->room;
            }

            {
                struct SessionRoom new = {
                    .addr = session->addr,
                    .room = session->room->id,
                };

                if (hash_set_addr_insert(manager->sessions, &new) == -1)
                    ; // TODO
            }

            {
                struct AddrSession new = {
                    .addr = session->addr,
                    .session = session
                };

                if (hash_set_addr_insert(session->room->sessions, &new) == -1)
                    ; // TODO
            }

            session->event = bufferevent_socket_new(manager->worker.base, command.fd, 0);
            if (session->event == NULL)
                ; // TODO
            session->event = bufferevent_filter_new(session->event, input_filter, output_filter, 0, NULL, session);
            if (session->event == NULL)
                ; // TODO
            bufferevent_setcb(session->event, on_session_read, NULL, on_session_event, session);

            session->supervisor = manager;
            session->writeEnable = false;

            bool ret = manager->onClientJoin(session, manager->worker.userData);
            if (ret) {
                bufferevent_enable(session->event, EV_READ | EV_WRITE);
            } else {
                destroy_session(session, false);
            }
        }
        break;

        case WORKER_COMMAND_KICK: {
            struct SessionRoom *session_room = hash_set_addr_get(manager->sessions, (void *)&command.addr);
            if (session_room != NULL) {
                // TODO: If session_room is NULL, transfer the command to the correct thread
                struct Room *room = ((struct RoomId *)hash_set_u32_get(manager->rooms, session_room->room))->room;
                struct Session *session = ((struct AddrSession *)hash_set_addr_get(room->sessions, (void *)&command.addr))->session;
                session->token = 0;
                kick_session(session, true);
            } else {
                mtx_lock(manager->worker.sessionsLock);
                if (hash_set_addr_get(manager->worker.sessions, (void *)&command.addr) != NULL) {
                    uint32_t room_id = ((struct SessionRoom *)hash_set_addr_get(manager->worker.sessions, (void *)&command.addr))->room;
                    mtx_unlock(manager->worker.sessionsLock);

                    mtx_lock(manager->worker.roomMapLock);
                    if (hash_set_u32_get(manager->worker.roomMap, room_id) != NULL) {
                        size_t thread = ((struct RoomThread *)hash_set_u32_get(manager->worker.roomMap, room_id))->thread;
                        mtx_unlock(manager->worker.roomMapLock);

                        mtx_lock(&manager->worker.transportMuteces[thread]);
                        write(manager->worker.transportSinks[thread], &command, sizeof(struct WorkerCommand));
                        mtx_unlock(&manager->worker.transportMuteces[thread]);
                    } else {
                        mtx_unlock(manager->worker.roomMapLock);
                    }
                } else {
                    mtx_unlock(manager->worker.sessionsLock);
                }
            }
        }
        break;
        }

    } else {
        manager->worker.onLog(LOG_ERR, "Reading a new client fd failed with %d: %s. Client's file descriptor leaked.\n", errno, strerror(errno));
    }
}

static enum bufferevent_filter_result input_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx)
{
    struct Session *session = ctx;
    uint32_t header;

    if (evbuffer_get_length(src) < 4)
        return BEV_NEED_MORE;

    evbuffer_copyout(src, &header, sizeof(uint32_t));
    uint16_t packet_len = (header >> 16) ^ header;
    // No need to convert to little-endian (the JVM uses big endian)
    //packet_len = (packet_len << 8) | (packet_len >> 8);
    if (4 + packet_len > evbuffer_get_length(src))
        return BEV_NEED_MORE;

    evbuffer_drain(src, 4);

    uint8_t *data = malloc(packet_len);
    if (data == NULL)
        return BEV_ERROR;

    evbuffer_remove(src, data, packet_len);
    decryption_context_decrypt(session->recieveContext, packet_len, data);
    evbuffer_add(dst, &packet_len, sizeof(uint16_t));
    evbuffer_add(dst, data, packet_len);
    free(data);

    return BEV_OK;
}

static enum bufferevent_filter_result output_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx)
{
    struct Session *session = ctx;
    size_t len = evbuffer_get_length(src);
    uint16_t packet_len;
    evbuffer_copyout(src, &packet_len, sizeof(uint16_t));

    if (2 + packet_len > len)
        return BEV_NEED_MORE;

    evbuffer_drain(src, 2);

    uint8_t data[65535];

    uint8_t header[4];
    encryption_context_header(session->sendContext, packet_len, header);

    evbuffer_remove(src, data, packet_len);

    printf("Sending packet with opcode %hd\n", ((uint16_t *)data)[0]);
    for (uint16_t i = 0; i < packet_len; i++)
        printf("%02X ", data[i]);
    printf("\n\n");

    encryption_context_encrypt(session->sendContext, packet_len, data);

    evbuffer_add(dst, header, 4);
    evbuffer_add(dst, data, packet_len);

    return BEV_OK;
}

static void on_greeter_user_fd_ready(int fd, short what, void *ctx)
{
    struct Session *session = ctx;
    struct OnResumeResult res = session->onResume(session, fd, libevent_to_poll(what));
    if (res.status <= 0) {
        event_free(session->userEvent);
        session->userEvent = NULL;
        if (res.status < 0) {
            if (session->state == SESSION_STATE_CONNECTING) {
                event_free(session->userEvent);
                destroy_pending_session(session, false);
            } else {
                kick_pending_session(session, false);
            }
        } else if (session->state == SESSION_STATE_CONNECTING) {
            finalize_session_connect(session);
        } else if (session->state == SESSION_STATE_KICKING) {
            kick_pending_session(session, false);
        } else if (session->state == SESSION_STATE_DISCONNECTING) {
            destroy_pending_session(session, false);
        } else if (res.room != -1) {
            session->targetRoom = res.room;
            if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0) {
                bufferevent_setcb(session->event, NULL, on_pending_session_write, on_pending_session_event, session);
            } else {
                void *temp = bufferevent_get_underlying(session->event);
                bufferevent_free(session->event);
                session->event = temp;
                if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0)
                    bufferevent_setcb(session->event, NULL, on_pending_session_write, on_pending_session_event, session);
                else
                    do_pending_assign(session);
            }
        }
    }
}

static void on_worker_user_fd_ready(int fd, short what, void *ctx)
{
    struct Session *session = ctx;
    struct OnResumeResult res = session->onResume(session, fd, libevent_to_poll(what));
    if (res.status <= 0) {
        event_free(session->userEvent);
        session->userEvent = NULL;

        if (session->state != SESSION_STATE_KICKING && session->state != SESSION_STATE_DISCONNECTING && res.status < 0) {
            kick_session(session, false);
        } else if (session->state == SESSION_STATE_KICKING) {
            kick_session(session, false);
        } else if (session->state == SESSION_STATE_DISCONNECTING) {
            destroy_session(session, false);
        } else if (res.room != -1) {
            session->targetRoom = res.room;
            if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0) {
                bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
            } else {
                void *temp = bufferevent_get_underlying(session->event);
                bufferevent_free(session->event);
                session->event = temp;
                if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0)
                    bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
                else
                    do_transfer(session);
            }
        } else {
            bufferevent_enable(session->event, EV_READ);
        }
    }
}

static void on_session_read(struct bufferevent *event, void *ctx)
{
    struct Session *session = ctx;
    struct RoomManager *manager = session->supervisor;
    struct evbuffer *input = bufferevent_get_input(event);
    // TODO: Maybe there is no one-to-one correspondence between on_session_read and input_filter
    // meaning the input buffer can contain multiple packets
    while (evbuffer_get_length(input) > 0) {
        uint16_t len;
        evbuffer_remove(input, &len, sizeof(uint16_t));
        uint8_t data[len];
        evbuffer_remove(input, data, len);
        struct OnPacketResult res = manager->worker.onClientPacket(session, len, data);
        if (res.status <= 0) {
            if (res.status < 0) {
                kick_session(session, true);
                break;
            } else if (res.room != -1) {
                session->targetRoom = res.room;
                if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0) {
                    bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
                } else {
                    void *temp = bufferevent_get_underlying(session->event);
                    bufferevent_free(session->event);
                    session->event = temp;
                    if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0)
                        bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
                    else
                        do_transfer(session);
                }
                break;
            }
        }
    }
}

static void on_pending_session_read(struct bufferevent *event, void *ctx)
{
    struct Session *session = ctx;
    struct ChannelServer *server = session->supervisor;
    struct evbuffer *input = bufferevent_get_input(event);
    // TODO: Maybe there is no one-to-one correspondence between on_session_read and input_filter
    // meaning the input buffer can contain multiple packets
    uint16_t len;
    evbuffer_remove(input, &len, sizeof(uint16_t));
    uint8_t data[len];
    evbuffer_remove(input, data, len);
    struct OnPacketResult res = server->worker.onClientPacket(session, len, data);
    if (res.status <= 0) {
        if (res.status < 0) {
            kick_pending_session(session, false);
        } else if (res.room != -1) {
            session->targetRoom = res.room;
            if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0) {
                bufferevent_setcb(session->event, NULL, on_pending_session_write, on_pending_session_event, session);
            } else {
                void *temp = bufferevent_get_underlying(session->event);
                bufferevent_free(session->event);
                session->event = temp;
                if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0)
                    bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
                else
                    do_pending_assign(session);
            }
        }
    }
}

static void on_session_write(struct bufferevent *event, void *ctx)
{
    struct Session *session = ctx;
    if (evbuffer_get_length(bufferevent_get_output(event)) == 0) {
        if (bufferevent_get_underlying(event) != NULL) {
            void *temp = bufferevent_get_underlying(event);
            bufferevent_free(event);
            event = temp;
        }
        if (evbuffer_get_length(bufferevent_get_output(event)) != 0)
            bufferevent_setcb(event, NULL, on_session_write, on_session_event, session);
        else
            do_transfer(session);
    }
}

static void on_pending_session_write(struct bufferevent *event, void *ctx)
{
    struct Session *session = ctx;
    if (evbuffer_get_length(bufferevent_get_output(event)) == 0) {
        if (bufferevent_get_underlying(event) != NULL) {
            void *temp = bufferevent_get_underlying(event);
            bufferevent_free(event);
            event = temp;
        }
        if (evbuffer_get_length(bufferevent_get_output(event)) != 0)
            bufferevent_setcb(event, NULL, on_pending_session_write, on_pending_session_event, session);
        else
            do_pending_assign(session);
    }
}

static void on_pending_session_event(struct bufferevent *event, short what, void *ctx)
{
    struct Session *session = ctx;

    if (what & BEV_EVENT_EOF || what & BEV_EVENT_ERROR) {
        kick_pending_session(session, false);
    }
}

static void on_session_event(struct bufferevent *event, short what, void *ctx)
{
    struct Session *session = ctx;

    if (what & BEV_EVENT_READING && (what & BEV_EVENT_EOF || what & BEV_EVENT_ERROR))
        kick_session(session, false);
    else if (what & BEV_EVENT_WRITING)
        shutdown(bufferevent_getfd(session->event), SHUT_RD);
}

static int start_worker(void *ctx_)
{
    struct RoomManager *manager = ctx_;

    event_base_dispatch(manager->worker.base);

    event_heap_destroy(&manager->heap);
    int fd = event_get_fd(manager->timer);
    event_free(manager->timer);
    close(fd);
    hash_set_addr_destroy(manager->sessions);
    hash_set_u32_destroy(manager->rooms);
    manager->worker.destroyContext(manager->worker.userData);
    event_base_free(manager->worker.base);
    free(manager);

    return 0;
}

static struct Session *create_session(struct ChannelServer *server, int fd, struct sockaddr *addr, int socklen)
{
    struct Session *session = malloc(sizeof(struct Session));
    if (session == NULL)
        return NULL;

    session->supervisor = server;
    memcpy(&session->addr, addr, socklen);
    uint8_t iv[4] = { 0 };
    session->sendContext = encryption_context_new(iv, ~MAPLE_VERSION);
    if (session->sendContext == NULL)
        goto free_slot;
    session->recieveContext = decryption_context_new(iv);
    if (session->recieveContext == NULL)
        goto destroy_send_context;
    session->event = bufferevent_socket_new(server->worker.base, fd, 0);
    if (session->event == NULL)
        goto destroy_recieve_context;

    bufferevent_enable(session->event, EV_WRITE);

    struct AddrSession new = {
        .session = session
    };

    memcpy(&new.addr, addr, socklen);
    if (hash_set_addr_insert(server->sessions, &new) == -1)
        goto destroy_event;

    session->token = 0;
    session->room = NULL;
    session->userEvent = NULL;
    session->writeEnable = false;
    session->targetRoom = -1;

    return session;

destroy_event:
    bufferevent_free(session->event);
destroy_recieve_context:
    decryption_context_destroy(session->recieveContext);
destroy_send_context:
    encryption_context_destroy(session->sendContext);
free_slot:
    free(session);
    return NULL;
}

static struct Room *create_room(struct RoomManager *manager, uint32_t id)
{
    struct Room *room = malloc(sizeof(struct Room));
    if (room == NULL)
        return NULL;

    room->timers = malloc(sizeof(struct TimerHandle *));
    if (room->timers == NULL) {
        free(room);
        return NULL;
    }

    room->sessions = hash_set_addr_create(sizeof(struct AddrSession), offsetof(struct AddrSession, addr));
    if (room->sessions == NULL) {
        free(room->sessions);
        free(room);
        return NULL;
    }

    room->id = id;
    room->manager = manager;
    room->timerCapacity = 1;
    room->timerCount = 0;

    return room;
}

static int finalize_session_connect(struct Session *session)
{
    session->state = SESSION_STATE_CONNECTED;
    const uint8_t *recv_iv = decryption_context_get_iv(session->recieveContext);
    const uint8_t *send_iv = encryption_context_get_iv(session->sendContext);
    uint8_t data[16] = {
        0x0E,
        0x00,
        MAPLE_VERSION,
        0x00,
        0x01,
        0x00,
        49,
        recv_iv[0],
        recv_iv[1],
        recv_iv[2],
        recv_iv[3],
        send_iv[0],
        send_iv[1],
        send_iv[2],
        send_iv[3],
        8
    };

    // Write without encryption
    if (bufferevent_write(session->event, data, 16) == -1)
        return -1;

    void *temp = bufferevent_filter_new(session->event, input_filter, output_filter, 0, NULL, session);
    if (temp == NULL)
        return -1;

    session->event = temp;

    bufferevent_setcb(session->event, on_pending_session_read, NULL, on_pending_session_event, session);
    if (bufferevent_enable(session->event, EV_READ | EV_WRITE) == -1)
        return -1;

    return 0;
}

static void destroy_pending_session(struct Session *session, bool async)
{
    struct ChannelServer *server = session->supervisor;
    struct bufferevent *underlying = bufferevent_get_underlying(session->event);
    int fd = bufferevent_getfd(session->event);
    bufferevent_free(session->event);
    bufferevent_free(underlying);
    close(fd);
    encryption_context_destroy(session->sendContext);
    decryption_context_destroy(session->recieveContext);

    if (session->token != 0) {
        uint8_t data[5];
        data[0] = 1;
        memcpy(data + 1, &session->token, 4);
        mtx_lock(server->worker.mtx);
        if (!*server->worker.connected || bufferevent_write(*server->worker.login, data, 5) == -1) {
            struct LoggedOutNode *new = malloc(sizeof(struct LoggedOutNode));
            if (new != NULL) {
                new->next = *server->worker.head;
                new->token = session->token;
                *server->worker.head = new;
            }
        }
        mtx_unlock(server->worker.mtx);
    }

    if (!async)
        hash_set_addr_remove(server->sessions, (void *)&session->addr);

    free(session);
}

static void destroy_session(struct Session *session, bool async)
{
    struct RoomManager *manager = session->supervisor;
    struct Room *room = session->room;
    int fd = bufferevent_getfd(session->event);
    struct bufferevent *underlying = bufferevent_get_underlying(session->event);
    bufferevent_free(session->event);
    bufferevent_free(underlying);
    close(fd);
    encryption_context_destroy(session->sendContext);
    decryption_context_destroy(session->recieveContext);

    hash_set_addr_remove(manager->sessions, (void *)&session->addr);

    mtx_lock(manager->worker.sessionsLock);
    hash_set_addr_remove(manager->worker.sessions, (void *)&session->addr);
    mtx_unlock(manager->worker.sessionsLock);

    if (session->token != 0) {
        uint8_t data[5];
        data[0] = 1;
        memcpy(data + 1, &session->token, 4);
        mtx_lock(manager->worker.mtx);
        if (!*manager->worker.connected || bufferevent_write(*manager->worker.login, data, 5) == -1) {
            struct LoggedOutNode *new = malloc(sizeof(struct LoggedOutNode));
            if (new != NULL) {
                new->next = *manager->worker.head;
                new->token = session->token;
                *manager->worker.head = new;
            }
        }
        mtx_unlock(manager->worker.mtx);
    }

    if (!async) {
        hash_set_addr_remove(room->sessions, (void *)&session->addr);
        if (hash_set_addr_size(room->sessions) == 0)
            destroy_room(manager, room, false);
    }

    free(session);
}

static bool kick_common(struct Worker *worker, struct Session *session, void (*destroy)(struct Session *session, bool async), bool async);

static bool kick_pending_session(struct Session *session, bool async)
{
    return kick_common(&((struct ChannelServer *)session->supervisor)->worker, session, destroy_pending_session, async);
}

static bool kick_session(struct Session *session, bool async)
{
    return kick_common(&((struct RoomManager *)session->supervisor)->worker, session, destroy_session, async);
}

static bool kick_common(struct Worker *worker, struct Session *session, void (*destroy)(struct Session *session, bool async), bool async)
{
    if (session->userEvent == NULL) {
        worker->onClientDisconnect(session);
        if (session->userEvent == NULL) {
            destroy(session, async);
            return true;
        } else {
            bufferevent_disable(session->event, EV_READ);
            session->state = SESSION_STATE_DISCONNECTING;
        }
    } else if (session->state != SESSION_STATE_DISCONNECTING) {
        session->state = SESSION_STATE_KICKING;
    }

    return false;
}

static void destroy_room(struct RoomManager *manager, struct Room *room, bool async)
{
    for (size_t i = 0; i < room->timerCount; i++) {
        if (!room->timers[i]->keepAlive) {
            if (event_heap_remove(&manager->heap, room->timers[i]->node)) {
                struct TimerEvent *next = event_heap_top(&manager->heap);
                if (next != NULL) {
                    struct itimerspec spec = { .it_value = next->time, .it_interval = { 0, 0 } };
                    timerfd_settime(event_get_fd(manager->timer), TFD_TIMER_ABSTIME, &spec, NULL);
                } else {
                    struct itimerspec spec = { .it_value = { 0, 0 } };
                    timerfd_settime(event_get_fd(manager->timer), 0, &spec, NULL);
                }
            }
            struct TimerHandle *timer = room->timers[i];
            room->timers[i] = room->timers[room->timerCount - 1];
            room->timers[i]->index = i;
            free(timer);
            room->timerCount--;
            i--;
        }
    }

    if (room->timerCount == 0) {
        manager->onRoomDestroy(room);
        free(room->timers);

        // If the room is being destroyed while iterating over the manager->rooms set then
        // we must not modify that set
        if (!async)
            hash_set_u32_remove(manager->rooms, room->id);

        hash_set_addr_destroy(room->sessions);

        mtx_lock(manager->worker.roomMapLock);
        hash_set_u32_remove(manager->worker.roomMap, room->id);
        mtx_unlock(manager->worker.roomMapLock);

        free(room);
    }
}

static void sift_up(struct TimerEventHeap *heap, size_t i);
static void sift_down(struct TimerEventHeap *heap, size_t i);

static int event_heap_init(struct TimerEventHeap *heap)
{
    heap->events = malloc(sizeof(struct TimerEvent));
    if (heap->events == NULL)
        return -1;

    heap->capacity = 1;
    heap->count = 0;

    return 0;
}

static void event_heap_destroy(struct TimerEventHeap *heap)
{
    free(heap->events);
}

static int event_heap_push(struct TimerEventHeap *heap, struct timespec tp, void (*f)(struct Room *, struct TimerHandle *), struct TimerHandle *handle)
{
    if (heap->count == heap->capacity) {
        void *temp = realloc(heap->events, (heap->capacity * 2) * sizeof(struct TimerEvent));
        if (temp == NULL)
            return -1;

        heap->events = temp;
        for (size_t i = 0; i < heap->count; i++)
            heap->events[i].handle->node = &heap->events[i];

        heap->capacity *= 2;
    }

    heap->events[heap->count].time = tp;
    heap->events[heap->count].f = f;
    heap->events[heap->count].handle = handle;

    handle->node = &heap->events[heap->count];

    heap->count++;

    sift_up(heap, heap->count - 1);

    return 0;
}

static struct TimerEvent *event_heap_top(struct TimerEventHeap *heap)
{
    if (heap->count == 0)
        return NULL;

    return &heap->events[0];
}

static struct TimerEvent event_heap_removetop(struct TimerEventHeap *heap)
{
    struct TimerEvent ret = heap->events[0];

    heap->events[0] = heap->events[heap->count - 1];
    heap->events[0].handle->node = &heap->events[0];
    heap->count--;

    sift_down(heap, 0);

    return ret;
}

static bool event_heap_remove(struct TimerEventHeap *heap, struct TimerEvent *node)
{
    ptrdiff_t i = node - heap->events;

    bool ret = i == 0;

    heap->count--;
    heap->events[i] = heap->events[heap->count];
    heap->events[i].handle->node = &heap->events[i];
    if (i == 0 || timespec_cmp(&heap->events[(i-1) / 2].time, &heap->events[i].time) < 0)
        sift_down(heap, i);
    else
        sift_up(heap, i);

    return ret;
}

static void swap(struct TimerEventHeap *heap, size_t i, size_t j);

static void sift_up(struct TimerEventHeap *heap, size_t i)
{
    while (i > 0) {
        if (timespec_cmp(&heap->events[(i-1) / 2].time, &heap->events[i].time) > 0) {
            swap(heap, (i-1) / 2, i);
            i = (i-1) / 2;
        } else {
            break;
        }
    }
}

static void sift_down(struct TimerEventHeap *heap, size_t i)
{
    while (i*2 + 1 < heap->count) {
        if (i*2 + 2 == heap->count) {
            if (timespec_cmp(&heap->events[i].time, &heap->events[i*2 + 1].time) > 0)
                swap(heap, i, i*2 + 1);

            break;
        }

        if (timespec_cmp(&heap->events[i].time, &heap->events[i*2 + 1].time) <= 0 && timespec_cmp(&heap->events[i].time, &heap->events[i*2 + 2].time) <= 0)
            break;

        if (timespec_cmp(&heap->events[i].time, &heap->events[i*2 + 1].time) > 0 && timespec_cmp(&heap->events[i].time, &heap->events[i*2 + 2].time) > 0) {
            if (timespec_cmp(&heap->events[i*2 + 1].time, &heap->events[i*2 + 2].time) < 0) {
                swap(heap, i, i*2 + 1);
                i = i*2 + 1;
            } else {
                swap(heap, i, i*2 + 2);
                i = i*2 + 2;
            }
        } else if (timespec_cmp(&heap->events[i].time, &heap->events[i*2 + 1].time) > 0) {
            swap(heap, i, i*2 + 1);
            i = i*2 + 1;
        } else {
            swap(heap, i, i*2 + 2);
            i = i*2 + 2;
        }
    }
}

static void swap(struct TimerEventHeap *heap, size_t i, size_t j)
{
    struct TimerEvent tmp = heap->events[i];
    heap->events[i] = heap->events[j];
    heap->events[i].handle->node = &heap->events[i];
    heap->events[j] = tmp;
    heap->events[j].handle->node = &heap->events[j];
}

static short poll_to_libevent(int mask)
{
    if (mask < 0)
        return mask;

    short ret = 0;
    if (mask & POLLIN)
        ret |= EV_READ;

    if (mask & POLLOUT)
        ret |= EV_WRITE;

    return ret;
}

static int libevent_to_poll(short mask)
{
    int ret = 0;
    if (mask & EV_READ)
        ret |= POLLIN;

    if (mask & EV_WRITE)
        ret |= POLLOUT;

    return ret;
}

static long timespec_cmp(struct timespec *tp1, struct timespec *tp2)
{
    if (tp1->tv_sec == tp2->tv_sec)
        return tp1->tv_nsec - tp2->tv_nsec;

    return tp1->tv_sec - tp2->tv_sec;
}

static void do_pending_assign(struct Session *session)
{
    struct ChannelServer *server = session->supervisor;
    struct WorkerCommand transfer = {
        .type = WORKER_COMMAND_NEW_CLIENT,
        .fd = bufferevent_getfd(session->event),
        .session = session,
    };

    struct bufferevent *event = session->event;
    struct sockaddr_storage addr = session->addr;
    uint32_t room_id = session->targetRoom;

    mtx_lock(server->worker.roomMapLock);
    size_t thread;
    if (hash_set_u32_get(server->worker.roomMap, room_id) == NULL) {
        // TODO: Pick the least busy thread
        struct RoomThread new = {
            .room = room_id,
            .thread = 0,
        };
        hash_set_u32_insert(server->worker.roomMap, &new);
        thread = 0;
    } else {
        thread = ((struct RoomThread *)hash_set_u32_get(server->worker.roomMap, room_id))->thread;
    }

    mtx_unlock(server->worker.roomMapLock);

    struct SessionRoom new = {
        .addr = addr,
        .room = room_id
    };

    mtx_lock(server->worker.sessionsLock);
    hash_set_addr_insert(server->worker.sessions, (void *)&new);
    mtx_unlock(server->worker.sessionsLock);

    bool sent;
    mtx_lock(&server->worker.transportMuteces[thread]);
    sent = write(server->worker.transportSinks[thread], &transfer, sizeof(struct WorkerCommand)) != -1;
    mtx_unlock(&server->worker.transportMuteces[thread]);

    if (sent) {
        bufferevent_free(event);
        hash_set_addr_remove(server->sessions, (void *)&addr);
    } else {
        kick_pending_session(session, false);
    }
}

static void do_transfer(struct Session *session)
{
    struct RoomManager *manager = session->supervisor;

    struct WorkerCommand transfer = {
        .type = WORKER_COMMAND_NEW_CLIENT,
        .fd = bufferevent_getfd(session->event),
        .session = session,
    };

    struct Room *room = session->room;

    size_t thread;
    mtx_lock(manager->worker.roomMapLock);
    if (hash_set_u32_get(manager->worker.roomMap, session->targetRoom) == NULL) {
        // TODO: Pick the least busy thread
        struct RoomThread new = {
            .room = session->targetRoom,
            .thread = 0,
        };
        hash_set_u32_insert(manager->worker.roomMap, &new);
        thread = 0;
    } else {
        thread = ((struct RoomThread *)hash_set_u32_get(manager->worker.roomMap, session->targetRoom))->thread;
    }
    mtx_unlock(manager->worker.roomMapLock);

    int sink = manager->worker.transportSinks[thread];

    struct bufferevent *event = session->event;
    struct sockaddr_storage addr = session->addr;
    uint32_t room_id = session->targetRoom;

    bool sent;
    mtx_lock(&manager->worker.transportMuteces[thread]);
    sent = write(sink, &transfer, sizeof(struct WorkerCommand)) != -1;
    mtx_unlock(&manager->worker.transportMuteces[thread]);

    if (sent) {
        bufferevent_free(event);
        hash_set_addr_remove(room->sessions, (void *)&addr);
        if (hash_set_addr_size(room->sessions) == 0)
            destroy_room(manager, room, false);

        hash_set_addr_remove(manager->sessions, (void *)&addr);

        mtx_lock(manager->worker.sessionsLock);
        // By this time the client could be kicked by the other thread,
        // so we check if it still exists before changing its room
        if (hash_set_addr_get(manager->worker.sessions, (void *)&addr) != NULL)
            ((struct SessionRoom *)hash_set_addr_get(manager->worker.sessions, (void *)&addr))->room = room_id;

        mtx_unlock(manager->worker.sessionsLock);
    } else {
        kick_session(session, false);
    }
}


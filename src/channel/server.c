#include "server.h"

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include "thread-coordinator.h"

#include "../crypt.h"
#include "../hash-map.h"

#define MAPLE_VERSION 83

#define TIMER_FREQ 10

struct Session {
    struct sockaddr_storage addr;
    void *supervisor;
    uint32_t id;
    struct Room *room;
    struct bufferevent *event;
    struct event *timer;
    struct timeval time;
    struct EncryptionContext *sendContext;
    struct DecryptionContext *recieveContext;
    struct event *userEvent;
    OnResume *onResume;
    bool writeEnable;
    uint32_t targetRoom;
    void *userData;
    jmp_buf jmp;
    bool disconnecting;
};

static void shutdown_session(struct Session *session);
static enum bufferevent_filter_result input_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx);
static enum bufferevent_filter_result output_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx);
static void on_session_read(struct bufferevent *event, void *ctx_);
static void on_pending_session_read(struct bufferevent *event, void *ctx_);
static void on_session_write(struct bufferevent *event, void *ctx_);
static void on_session_timer_expired(int fd, short what, void *ctx);

static void on_session_event(struct bufferevent *event, short what, void *ctx);
static void on_pending_session_event(struct bufferevent *event, short what, void *ctx);

struct RoomThread {
    uint32_t room;
    size_t thread;
};

struct LoggedOutNode {
    struct LoggedOutNode *next;
    uint32_t token;
};

struct Listener {
    void (*listener)(void *);
    void *ctx;
};

struct Property {
    uint32_t id;
    int32_t value;
    mtx_t mtx;
    size_t eventCount;
    struct event **events;
};

struct Event {
    struct HashSetU32 *properties;
    struct event *timer;
    void (*f)(struct Event *, void *);
    void *ctx;
};

struct UserEvent {
    struct event *event;
    OnResumeEvent *onResume;
    void *ctx;
};

static void on_event(int fd, short status, void *ctx);

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

    struct MapThreadCoordinator *coordinator;

    // Global set of connected sessions and the room they are in right now
    mtx_t *sessionsLock;
    struct HashSetU32 *sessions; // Uses `struct SessionRoom`

    mtx_t *lock;
    bool *connected;
    struct bufferevent **login;
    struct LoggedOutNode **head;
};

struct AddrSession {
    struct sockaddr_storage addr;
    struct Session *session;
};

struct IdSession {
    uint32_t id;
    struct Session *session;
};

struct ChannelServer {
    struct Worker worker;
    OnClientConnect *onClientConnect;

    /// Worker thread handles
    size_t threadCount;
    thrd_t *threads;

    size_t eventCount;
    struct Event *events;

    // Sessions that still don't have a room assigned, uses `struct AddrSession`
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

struct TimerHandle {
    struct Room *room;
    size_t index;
    void *data;
    void (*f)(struct Room *room, struct TimerHandle *handle);
    struct event *event;
};

struct Room {
    uint32_t id;
    struct RoomManager *manager;
    struct HashSetU32 *sessions; // Uses struct IdSession
    size_t timerCapacity;
    size_t timerCount;
    struct TimerHandle **timers;
    void *userData;
    OnRoomResume *onResume;
    bool keepAlive;
};

static struct Room *create_room(struct RoomManager *manager, uint32_t id);

struct RoomId {
    uint32_t id;
    struct Room *room;
};

struct SessionRoom {
    uint32_t id;
    uint32_t room;
};

// TODO: Maybe use a thread_local global instead of passing the struct between event callbacks
struct RoomManager {
    struct Worker worker;
    OnClientJoin *onClientJoin;
    OnResume *onResumeClientJoin;
    OnRoomCreate *onRoomCreate;
    OnRoomDestroy *onRoomDestroy;

    // Used when session_send_command() is called to indicate if the command was successfully sent to the target
    OnClientTimer *onClientTimer;
    OnClientCommand *onClientCommand;

    struct HashSetU32 *rooms; // Uses `RoomId`
    void *userData;
};

enum WorkerCommandType {
    WORKER_COMMAND_NEW_CLIENT,
    WORKER_COMMAND_KICK,
    WORKER_COMMAND_USER_COMMAND,
};

struct WorkerCommand {
    enum WorkerCommandType type;
    union {
        // NEW_CLIENT
        struct {
            struct Session *session;
        } new;
        // KICK
        struct {
            uint32_t id;
        } kick;
        // USER_COMMAND
        struct {
            uint32_t id;
            uint32_t target;
            sem_t sem;
            int res;
            void *ctx;
        } user;
    };
};

static int start_worker(void *ctx_);

static void on_worker_command(int fd, short what, void *ctx_);
static void on_user_fd_ready(int fd, short what, void *ctx);
static void on_pending_session_user_fd_ready(int fd, short what, void *ctx);
static void on_session_user_fd_ready(int fd, short what, void *ctx);

static struct Session *create_session(struct ChannelServer *server, int fd, struct sockaddr *addr, int socklen);
static void destroy_pending_session(struct Session *session);
static void destroy_session(struct Session *session);
static void kick_common(struct Worker *worker, struct Session *session, void (*destroy)(struct Session *session));
static void destroy_room(struct Room *room);

static short poll_to_libevent(int mask);
static int libevent_to_poll(short mask);

static void do_transfer(struct Session *session);

struct ChannelServer *channel_server_create(uint16_t port, OnLog *on_log, const char *host, CreateUserContext *create_user_context, DestroyUserContext destroy_user_ctx, OnClientConnect *on_client_connect, OnClientDisconnect *on_client_disconnect, OnClientJoin *on_client_join, OnClientPacket *on_pending_client_packet, OnClientPacket *on_client_packet, OnRoomCreate *on_room_create, OnRoomDestroy *on_room_destroy, OnClientCommand on_client_command, OnClientTimer on_client_timer, void *global_ctx, size_t event_count)
{
    struct ChannelServer *server = malloc(sizeof(struct ChannelServer));
    if (server == NULL)
        return NULL;

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

    server->pendings = hash_set_u32_create(sizeof(uint32_t), 0);

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port),
    };

    server->worker.listener = evconnlistener_new_bind(server->worker.base, on_session_connect, server, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, (const struct sockaddr *)&addr, sizeof(struct sockaddr_in));
    if (server->worker.listener == NULL)
        goto free_base;

    server->worker.coordinator = map_thread_coordinator_create();

    server->worker.sessions = hash_set_u32_create(sizeof(struct SessionRoom), offsetof(struct SessionRoom, id));
    server->worker.sessionsLock = malloc(sizeof(mtx_t));
    mtx_init(server->worker.sessionsLock, mtx_plain);

    server->worker.lock = malloc(sizeof(mtx_t));
    mtx_init(server->worker.lock, mtx_plain);

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
    server->commandEvent = event_new(server->worker.base,
                                     pipefds[0],
                                     EV_READ | EV_PERSIST,
                                     on_command,
                                     server);
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

        if (mtx_init(&server->worker.transportMuteces[server->threadCount],
                     mtx_plain) != thrd_success) {
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

        manager->worker.transportEvent = event_new(manager->worker.base,
                                                   pair[0],
                                                   EV_READ | EV_PERSIST,
                                                   on_worker_command,
                                                   manager);
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

        manager->rooms = hash_set_u32_create(sizeof(struct RoomId),
                                            offsetof(struct RoomId, id));
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

        manager->worker.onLog = on_log;
        manager->worker.onClientDisconnect = on_client_disconnect;
        manager->worker.onClientPacket = on_client_packet;
        manager->worker.transportMuteces = server->worker.transportMuteces;
        manager->worker.transportSinks = server->worker.transportSinks;
        manager->worker.destroyContext = destroy_user_ctx;
        manager->worker.coordinator = server->worker.coordinator;
        manager->worker.sessionsLock = server->worker.sessionsLock;
        manager->worker.sessions = server->worker.sessions;
        manager->worker.lock = server->worker.lock;
        manager->worker.login = server->worker.login;
        manager->worker.connected = server->worker.connected;
        manager->worker.head = server->worker.head;
        manager->onClientJoin = on_client_join;
        manager->onRoomCreate = on_room_create;
        manager->onRoomDestroy = on_room_destroy;
        manager->onClientCommand = on_client_command;
        manager->onClientTimer = on_client_timer;
        manager->userData = global_ctx;

        if (thrd_create(server->threads + server->threadCount, start_worker, manager) != thrd_success) {
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

    server->events = malloc(event_count * sizeof(struct Event));
    if (server->events == NULL)
        goto exit_threads;

    for (server->eventCount = 0;
         server->eventCount < event_count;
         server->eventCount++) {
        struct Event *event = &server->events[server->eventCount];
        event->timer = evtimer_new(server->worker.base, on_event, event);
        if (event->timer == NULL)
            goto free_events;

        event->properties = hash_set_u32_create(sizeof(struct Property),
                                               offsetof(struct Property, id));
        if (event->properties == NULL) {
            event_free(event->timer);
            goto free_events;
        }
    }

    server->userData = global_ctx;
    server->first = 0;

    return server;

free_events:
    for (size_t i = 0; i < server->eventCount; i++)
        event_free(server->events[i].timer);

    free(server->events);

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

static void do_destroy_property(void *data, void *ctx)
{
    struct Property *property = data;
    mtx_destroy(&property->mtx);
    free(property->events);
}

void channel_server_destroy(struct ChannelServer *server)
{
    for (size_t i = 0; i < server->eventCount; i++) {
        hash_set_u32_foreach(server->events[i].properties, do_destroy_property, NULL);
        hash_set_u32_destroy(server->events[i].properties);
    }
    free(server->events);

    for (size_t i = 0; i < server->threadCount; i++)
        mtx_destroy(&server->worker.transportMuteces[i]);

    mtx_destroy(server->worker.lock);
    free(server->worker.lock);
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
    map_thread_coordinator_destroy(server->worker.coordinator);
    hash_set_addr_destroy(server->sessions);
    hash_set_u32_destroy(server->worker.sessions);
    event_base_free(server->worker.base);
    server->worker.destroyContext(server->worker.userData);
    free(server->worker.transportMuteces);
    free(server->worker.transportSinks);
    free(server);
}

struct Event *channel_server_get_event(struct ChannelServer *server, size_t event)
{
    return &server->events[event];
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

static enum bufferevent_filter_result input_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx);
static enum bufferevent_filter_result output_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx);

static void on_pending_session_read(struct bufferevent *event, void *ctx_);
static void on_pending_session_event(struct bufferevent *event, short what, void *ctx);

bool session_accept(struct Session *session)
{
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
    if (bufferevent_write(session->event, data, 16) == -1) {
        struct ChannelServer *server = session->supervisor;
        int fd = bufferevent_getfd(session->event);
        bufferevent_free(session->event);
        close(fd);
        encryption_context_destroy(session->sendContext);
        decryption_context_destroy(session->recieveContext);

        hash_set_addr_remove(server->sessions, (void *)&session->addr);

        free(session);
        return false;
    }

    void *temp = bufferevent_filter_new(session->event, input_filter, output_filter, 0, NULL, session);
    if (temp == NULL) {
        struct ChannelServer *server = session->supervisor;
        int fd = bufferevent_getfd(session->event);
        bufferevent_free(session->event);
        close(fd);
        encryption_context_destroy(session->sendContext);
        decryption_context_destroy(session->recieveContext);

        hash_set_addr_remove(server->sessions, (void *)&session->addr);

        free(session);
        return false;
    }

    session->event = temp;

    bufferevent_setcb(session->event, on_pending_session_read, NULL, on_pending_session_event, session);
    if (bufferevent_enable(session->event, EV_READ | EV_WRITE) == -1) {
        struct ChannelServer *server = session->supervisor;
        struct bufferevent *underlying = bufferevent_get_underlying(session->event);
        int fd = bufferevent_getfd(underlying);
        bufferevent_free(session->event);
        bufferevent_free(underlying);
        close(fd);
        encryption_context_destroy(session->sendContext);
        decryption_context_destroy(session->recieveContext);

        hash_set_addr_remove(server->sessions, (void *)&session->addr);

        free(session);
        return false;
    }

    return true;
}

const struct sockaddr *session_get_addr(struct Session *session)
{
    return (void *)&session->addr;
}

bool session_assign_id(struct Session *session, uint32_t id)
{
    struct ChannelServer *server = session->supervisor;
    uint32_t *pending = hash_set_u32_get(server->pendings, id);
    if (pending == NULL)
        return false;

    hash_set_u32_remove(server->pendings, id);

    struct SessionRoom new = {
        .id = id,
        .room = -1
    };

    mtx_lock(server->worker.sessionsLock);
    bool inserted = hash_set_u32_insert(server->worker.sessions, &new) != -1;
    mtx_unlock(server->worker.sessionsLock);

    if (!inserted)
        return false;

    session->id = id;
    return true;
}

static void on_session_write(struct bufferevent *event, void *ctx_);
static void on_session_event(struct bufferevent *event, short what, void *ctx);

void session_change_room(struct Session *session, uint32_t id)
{
    assert(session->id != 0);
    session->targetRoom = id;
    // Make sure that the output buffer is flushed before changing rooms
    if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0) {
        bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
        bufferevent_disable(session->event, EV_READ);
    } else {
        void *temp = bufferevent_get_underlying(session->event);
        bufferevent_free(session->event);
        session->event = temp;
        if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0) {
            bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
            bufferevent_disable(session->event, EV_READ);
        } else {
            do_transfer(session);
        }
    }
}

void do_transfer(struct Session *session)
{
    // Since we got here from session_change_room() (or on_session_write() after it was scheduled by session_change_room())
    // then session->event must refer to a bufferevent_socket and not a bufferevent_filter
    // as the filter wrapper was freed during session_change_room() (or on_session_write())
    // and session->event was replaced by the underlying bufferevent_socket
    assert(bufferevent_get_underlying(session->event) == NULL);

    if (session->room == NULL) {
        // This is the first time session_change_room() has been called on this session
        struct ChannelServer *server = session->supervisor;
        struct WorkerCommand *transfer = malloc(sizeof(struct WorkerCommand));
        bool sent;
        struct sockaddr_storage addr = session->addr;
        uint32_t id = session->id;

        transfer->type = WORKER_COMMAND_NEW_CLIENT;
        transfer->new.session = session;

        session->timer = evtimer_new(server->worker.base, on_session_timer_expired, session);

        session->time.tv_sec = TIMER_FREQ;
        session->time.tv_usec = 0;

        bufferevent_disable(session->event, EV_READ | EV_WRITE);

        {
            ssize_t thread = map_thread_coordinator_ref(server->worker.coordinator, session->targetRoom);

            mtx_lock(&server->worker.transportMuteces[thread]);
            sent = write(server->worker.transportSinks[thread], &transfer, sizeof(struct WorkerCommand *)) != -1;
            mtx_unlock(&server->worker.transportMuteces[thread]);
        }

        if (sent) {
            struct SessionRoom new = {
                .id = id,
                .room = session->targetRoom
            };

            mtx_lock(server->worker.sessionsLock);
            hash_set_u32_insert(server->worker.sessions, &new);
            mtx_unlock(server->worker.sessionsLock);

            hash_set_addr_remove(server->sessions, (void *)&addr);
        } else {
            shutdown_session(session);
        }
    } else {
        struct RoomManager *manager = session->supervisor;
        struct Room *room = session->room;
        uint32_t id = session->id;
        uint32_t room_id = session->targetRoom;
        ssize_t thread;
        struct WorkerCommand *transfer = malloc(sizeof(struct WorkerCommand));
        bool sent;
        transfer->type = WORKER_COMMAND_NEW_CLIENT;
        transfer->new.session = session;

        bufferevent_disable(session->event, EV_READ | EV_WRITE);
        if (session->userEvent != NULL) {
            event_del(session->userEvent);
        } else {
            session->time = (struct timeval) {};
            evtimer_pending(session->timer, &session->time);
            event_del(session->timer);
            struct timeval now;
            event_base_gettimeofday_cached(manager->worker.base, &now);
            evutil_timersub(&session->time, &now, &session->time);
        }

        thread = map_thread_coordinator_ref(manager->worker.coordinator, session->targetRoom);

        mtx_lock(&manager->worker.transportMuteces[thread]);
        sent = write(manager->worker.transportSinks[thread], &transfer, sizeof(struct WorkerCommand *)) != -1;
        mtx_unlock(&manager->worker.transportMuteces[thread]);

        if (sent) {
            hash_set_u32_remove(room->sessions, id);
            if (room->timerCount == 0 && hash_set_u32_size(room->sessions) == 0 && !room->keepAlive)
                destroy_room(room);

            mtx_lock(manager->worker.sessionsLock);
            // By this time the client could be kicked by the other thread,
            // so we check if it still exists before changing its room
            if (hash_set_u32_get(manager->worker.sessions, id) != NULL)
                ((struct SessionRoom *)hash_set_u32_get(manager->worker.sessions, id))->room = room_id;

            mtx_unlock(manager->worker.sessionsLock);
        } else {
            shutdown_session(session);
        }
    }
}

void session_kick(struct Session *session)
{
    shutdown_session(session);
    longjmp(session->jmp, 1);
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
    if (session->userEvent != NULL) {
        event_free(session->userEvent);
    } else if (session->timer != NULL) {
        session->time = (struct timeval) {};
        evtimer_pending(session->timer, &session->time);
        evtimer_del(session->timer);
        struct timeval now;
        event_base_gettimeofday_cached(worker->base, &now);
        evutil_timersub(&session->time, &now, &session->time);
    }

    session->userEvent = event_new(worker->base, fd, poll_to_libevent(status) | EV_PERSIST, session->targetRoom != -1 ? on_session_user_fd_ready : on_pending_session_user_fd_ready, session);
    if (session->userEvent == NULL)
        return -1;

    if (event_add(session->userEvent, NULL) == -1) {
        event_free(session->userEvent);
        session->userEvent = NULL;
        return -1;
    }

    bufferevent_disable(session->event, EV_READ);

    session->onResume = on_resume;
    return 0;
}

struct UserEvent *session_add_event(struct Session *session, int status, int fd, OnResumeEvent *on_resume, void *ctx)
{
    struct Worker *worker = session->supervisor;

    struct UserEvent *event = malloc(sizeof(struct UserEvent));
    if (event == NULL)
        return NULL;

    event->event = event_new(worker->base, fd, poll_to_libevent(status), on_user_fd_ready, event);
    if (event->event == NULL) {
        free(event);
        return NULL;
    }

    if (event_add(event->event, NULL) == -1) {
        event_free(event->event);
        free(event);
        return NULL;
    }

    event->onResume = on_resume;
    event->ctx = ctx;

    return event;
}

int session_close_event(struct Session *session)
{
    int fd = event_get_fd(session->userEvent);
    event_free(session->userEvent);
    session->userEvent = NULL;

    if (session->timer != NULL)
        evtimer_add(session->timer, &session->time);

    return fd;
}

struct Room *session_get_room(struct Session *session)
{
    return session->room;
}

struct BroadcastContext {
    struct Session *session;
    size_t len;
    uint8_t *packet;
};

static void do_broadcast(void *data, void *ctx_)
{
    struct Session *current = ((struct IdSession *)data)->session;
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
    hash_set_u32_foreach(session->room->sessions, do_broadcast, &ctx);
}

struct ForeachContext {
    struct Session *session;
    void (*f)(struct Session *src, struct Session *dst, void *ctx);
    void *ctx;
};

static void shutdown_session(struct Session *session)
{
    shutdown(bufferevent_getfd(session->event), SHUT_RD);
}

static void do_foreach(void *data, void *ctx_)
{
    struct Session *current = ((struct IdSession *)data)->session;
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
    hash_set_u32_foreach(session->room->sessions, do_foreach, &ctx);
}

void session_enable_write(struct Session *session)
{
    session->writeEnable = true;
}

bool session_send_command(struct Session *session, uint32_t target, void *command)
{
    struct RoomManager *manager = session->supervisor;

    struct WorkerCommand *cmd = malloc(sizeof(struct WorkerCommand));
    cmd->type = WORKER_COMMAND_USER_COMMAND;
    cmd->user.ctx = command;

    uint32_t room;
    {
        mtx_lock(manager->worker.sessionsLock);
        struct SessionRoom *session_room = hash_set_u32_get(manager->worker.sessions, target);
        if (session_room == NULL) {
            mtx_unlock(manager->worker.sessionsLock);
            return false;
        }

        cmd->user.id = session_room->id;
        room = session_room->room;
        mtx_unlock(manager->worker.sessionsLock);
    }

    size_t thread = map_thread_coordinator_get(manager->worker.coordinator, room);

    // Can't fail under this circumstence
    sem_init(&cmd->user.sem, 0, 0);

    mtx_lock(&manager->worker.transportMuteces[thread]);
    bool sent = write(manager->worker.transportSinks[thread], &cmd, sizeof(struct WorkerCommand *)) != -1;
    mtx_unlock(&manager->worker.transportMuteces[thread]);

    if (sent) {
        sem_wait(&cmd->user.sem);
        sent = cmd->user.res;
    }

    free(cmd);
    return sent;
}

void *room_get_base(struct Room *room)
{
    return room->manager->worker.base;
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
    hash_set_u32_foreach(room->sessions, do_broadcast, &ctx);
}

void room_foreach(struct Room *room, void (*f)(struct Session *src, struct Session *dst, void *ctx), void *ctx_)
{
    struct ForeachContext ctx = {
        .session = NULL,
        .f = f,
        .ctx = ctx_
    };
    hash_set_u32_foreach(room->sessions, do_foreach, &ctx);
}

void room_keep_alive(struct Room *room)
{
    room->keepAlive = true;
}

void room_break_off(struct Room *room)
{
    room->keepAlive = false;
    if (room->timerCount == 0 && hash_set_u32_size(room->sessions) == 0)
        destroy_room(room);
}

void event_set_property(struct Event *event, uint32_t property, int32_t value)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    if (prop == NULL) {
        struct Property prop = {
            .id = property,
            .value = value,
            .eventCount = 0,
            .events = NULL
        };
        mtx_init(&prop.mtx, mtx_plain);
        hash_set_u32_insert(event->properties, &prop);
    } else {
        mtx_lock(&prop->mtx);
        prop->value = value;
        for (size_t i = 0; i < prop->eventCount; i++) {
            if (prop->events[i] != NULL)
                evuser_trigger(prop->events[i]);
        }
        mtx_unlock(&prop->mtx);
    }
}

bool event_has_property(struct Event *event, uint32_t property)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    return prop != NULL;
}

int32_t event_get_property(struct Event *event, uint32_t property)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    return prop->value;
}

void on_trigger_event(int fd, short what, void *ctx)
{
    struct Listener *listener = ctx;
    listener->listener(listener->ctx);
}

uint32_t event_add_listener(struct Event *event, void *base, uint32_t property, void (*f)(void *), void *ctx)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    struct Listener *listener = malloc(sizeof(struct Listener));
    if (listener == NULL)
        ; // TODO

    listener->listener = f;
    listener->ctx = ctx;

    struct event *new = event_new(base, -1, EV_PERSIST, on_trigger_event, listener);
    event_add(new, NULL);

    mtx_lock(&prop->mtx);
    void *temp = realloc(prop->events, (prop->eventCount + 1) * sizeof(struct event *));
    if (temp == NULL)
        ; // TODO

    prop->events = temp;
    prop->events[prop->eventCount] = new;
    prop->eventCount++;
    mtx_unlock(&prop->mtx);

    return prop->eventCount - 1;
}

void event_remove_listener(struct Event *event, uint32_t property, uint32_t listener_id)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    mtx_lock(&prop->mtx);

    void *listener;
    event_get_assignment(prop->events[listener_id], NULL, NULL, NULL, NULL, &listener);
    struct event *e = prop->events[listener_id];
    prop->events[listener_id] = NULL;
    mtx_unlock(&prop->mtx);

    free(listener);
    event_free(e);
}

void event_schedule(struct Event *e, void f(struct Event *, void *), void *ctx, const struct timespec *tm)
{
    struct timeval spec = {
        .tv_sec = tm->tv_sec,
        .tv_usec = tm->tv_nsec * 1000
    };
    evtimer_add(e->timer, &spec);

    e->f = f;
    e->ctx = ctx;
}

struct TimerHandle *room_add_timer(struct Room *room, uint64_t msec, void (*f)(struct Room *, struct TimerHandle *), void *data)
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
    handle->f = f;

    struct timeval tv = {
        .tv_usec = (msec % 1000) * 1000,
        .tv_sec = msec / 1000
    };

    handle->event = evtimer_new(manager->worker.base, on_timer_expired, handle);
    if (handle->event == NULL) {
    }

    if (evtimer_add(handle->event, &tv) == -1) {
    }

    room->timers[room->timerCount] = handle;
    room->timerCount++;

    return handle;
}

void room_stop_timer(struct TimerHandle *timer)
{
    struct Room *room = timer->room;
    room->timers[timer->index] = room->timers[room->timerCount - 1];
    room->timers[timer->index]->index = timer->index;
    room->timerCount--;
    event_free(timer->event);
    free(timer);

    if (room->timerCount == 0 && hash_set_u32_size(room->sessions) == 0 && !room->keepAlive)
        destroy_room(room);
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

struct UserEvent *user_event_add_event(struct UserEvent *ev, int status, int fd, OnResumeEvent *on_resume, void *ctx)
{
    struct event_base *base = event_get_base(ev->event);

    struct UserEvent *event = malloc(sizeof(struct UserEvent));
    if (event == NULL)
        return NULL;

    event->event = event_new(base, fd, poll_to_libevent(status), on_user_fd_ready, event);
    if (event->event == NULL) {
        free(event);
        return NULL;
    }

    if (event_add(event->event, NULL) == -1) {
        event_free(event->event);
        free(event);
        return NULL;
    }

    event->onResume = on_resume;
    event->ctx = ctx;

    return event;
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

static void do_pending_kick(void *data, void *ctx)
{
    struct Session *session = ((struct AddrSession *)data)->session;
    shutdown_session(session);
}

static void on_event(int fd, short status, void *ctx)
{
    struct Event *e = ctx;
    e->f(e, e->ctx);
}

static void on_command(int fd, short what, void *ctx)
{
    struct ChannelServer *server = ctx;
    struct Command command;
    ssize_t status = read(fd, &command, sizeof(struct Command));
    if (status == 0) {
        mtx_lock(server->worker.lock);
        *server->worker.connected = false;
        mtx_unlock(server->worker.lock);

        for (size_t i = 0; i < server->eventCount; i++) {
            event_free(server->events[i].timer);
        }

        if (*server->worker.login != NULL) {
            bufferevent_free(*server->worker.login);
        } else {
            evconnlistener_free(server->loginListener);
            if (server->isUnix)
                unlink(((struct sockaddr_un *)&server->addr)->sun_path);
        }

        event_free(server->commandEvent);
        close(fd);
        evconnlistener_free(server->worker.listener);
        server->worker.listener = NULL;
        hash_set_addr_foreach(server->sessions, do_pending_kick, NULL);
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

    server->onClientConnect(session, server->userData, server->worker.userData, addr);
}

static void on_login_server_read(struct bufferevent *bev, void *ctx);
static void on_login_server_write(struct bufferevent *bev, void *ctx);
static void on_login_server_event(struct bufferevent *bev, short what, void *ctx);

static void on_login_server_connect(struct evconnlistener *listener, evutil_socket_t fd,
        struct sockaddr *addr, int socklen, void *ctx)
{
    struct ChannelServer *server = ctx;
    // TODO: Check if addr is allowed
    *server->worker.login = bufferevent_socket_new(evconnlistener_get_base(listener), fd,
            BEV_OPT_CLOSE_ON_FREE);
    if (*server->worker.login == NULL) {
        close(fd);
        return;
    }

    evconnlistener_free(listener);
    if (server->isUnix)
        unlink(((struct sockaddr_un *)&server->addr)->sun_path);

    bufferevent_setcb(*server->worker.login, on_login_server_read, on_login_server_write, on_login_server_event, ctx);
    bufferevent_enable(*server->worker.login, EV_READ | EV_WRITE);

    bufferevent_write(*server->worker.login, &server->first, 1);
    if (server->first == 0) {
        server->first = 1;
        mtx_lock(server->worker.lock);
        *server->worker.connected = true;
        mtx_unlock(server->worker.lock);
    }
}

static void do_send_kick_command(void *data, void *ctx)
{
    struct SessionRoom *pair = data;
    struct ChannelServer *server = ctx;
    size_t thread = map_thread_coordinator_get(server->worker.coordinator, pair->room);

    if (thread != -1) {
        struct WorkerCommand *cmd = malloc(sizeof(struct WorkerCommand));
        cmd->type = WORKER_COMMAND_KICK,
            cmd->kick.id = pair->id,

        mtx_lock(&server->worker.transportMuteces[thread]);
        write(server->worker.transportSinks[thread], &cmd, sizeof(struct WorkerCommand *));
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
            hash_set_u32_foreach(server->worker.sessions, do_send_kick_command, server);
            mtx_unlock(server->worker.sessionsLock);

            mtx_lock(server->worker.lock);
            *server->worker.connected = true;
            mtx_unlock(server->worker.lock);
        } else {
            mtx_lock(server->worker.lock);
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
            mtx_unlock(server->worker.lock);
        }
    } else {
        while (evbuffer_get_length(bufferevent_get_input(bev)) >= 4) {
            // TODO: Time the pending character
            uint32_t id;
            evbuffer_remove(bufferevent_get_input(bev), &id, sizeof(uint32_t));

            hash_set_u32_insert(server->pendings, &id);

            uint8_t data[5];
            data[0] = 0;
            memcpy(data + 1, &id, sizeof(uint32_t));
            bufferevent_write(bev, data, 5);
        }
    }
}

static void on_login_server_write(struct bufferevent *bev, void *ctx)
{
    struct ChannelServer *server = ctx;
    mtx_lock(server->worker.lock);
    while (*server->worker.head != NULL) {
        struct LoggedOutNode *next = (*server->worker.head)->next;
        free(*server->worker.head);
        *server->worker.head = next;
    }
    mtx_unlock(server->worker.lock);
}

static void on_login_server_event(struct bufferevent *bev, short what, void *ctx)
{
    struct ChannelServer *server = ctx;
    server->loginListener = evconnlistener_new_bind(bufferevent_get_base(bev), on_login_server_connect, ctx, LEV_OPT_CLOSE_ON_FREE, 1, (void *)&server->addr, server->socklen);
    mtx_lock(server->worker.lock);
    *server->worker.connected = false;
    mtx_unlock(server->worker.lock);
    bufferevent_free(bev);
    *server->worker.login = NULL;
}

static void on_timer_expired(int fd, short what, void *ctx)
{
    struct TimerHandle *handle = ctx;
    struct Room *room = handle->room;
    handle->f(room, handle);
    room->timers[handle->index] = room->timers[room->timerCount - 1];
    room->timers[handle->index]->index = handle->index;
    room->timerCount--;
    if (room->timerCount == 0 && hash_set_u32_size(room->sessions) == 0 && !room->keepAlive)
        destroy_room(room);

    event_free(handle->event);
    free(handle);
}

static void do_kick(void *data, void *ctx)
{
    struct Session *session = ((struct IdSession *)data)->session;
    shutdown_session(session);
}

static void do_kill_room(void *data, void *ctx)
{
    struct Room *room = ((struct RoomId *)data)->room;

    if (hash_set_u32_size(room->sessions) != 0) {
        hash_set_u32_foreach(room->sessions, do_kick, room);
    } else {
        // ctx is the manager
        destroy_room(room);
    }
}

static void on_worker_command(int fd, short what, void *ctx_)
{
    struct RoomManager *manager = ctx_;
    ssize_t status;
    struct WorkerCommand *cmd;

    if ((status = read(fd, &cmd, sizeof(struct WorkerCommand *))) == 0) {
        // Shutdown request
        event_free(manager->worker.transportEvent);
        close(fd);

        hash_set_u32_foreach(manager->rooms, do_kill_room, manager);
    } else if (status != -1) {
        switch (cmd->type) {
        case WORKER_COMMAND_NEW_CLIENT: {
            struct Session *session = cmd->new.session;
            free(cmd);
            if (hash_set_u32_get(manager->rooms, session->targetRoom) == NULL) {
                session->room = create_room(manager, session->targetRoom);
                if (session->room == NULL) {
                    shutdown_session(session);
                    return;
                }

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
                struct IdSession new = {
                    .id = session->id,
                    .session = session
                };

                if (hash_set_u32_insert(session->room->sessions, &new) == -1)
                    ; // TODO
            }

            bufferevent_base_set(manager->worker.base, session->event);
            bufferevent_enable(session->event, EV_READ | EV_WRITE);
            session->event = bufferevent_filter_new(session->event, input_filter, output_filter, 0, NULL, session);
            if (session->event == NULL)
                ; // TODO
            bufferevent_setcb(session->event, on_session_read, NULL, on_session_event, session);

            if (session->userEvent != NULL) {
                event_base_set(manager->worker.base, session->userEvent);
                event_add(session->userEvent, NULL);
            } else {
                event_base_set(manager->worker.base, session->timer);
                evtimer_add(session->timer, &session->time);
            }

            session->supervisor = manager;
            // At this point the client hasn't necessaraly loaded the destination map
            // and as such sending map-related packets will cause them to crash
            // so we make sure that room_broadcast() won't send those packets
            // until we are sure that the client has loaded the map at which point,
            // session_enable_write() is called
            session->writeEnable = false;

            manager->onClientJoin(session, manager->worker.userData);
            bufferevent_enable(session->event, EV_READ | EV_WRITE);
        }
        break;

        case WORKER_COMMAND_KICK: {
            uint32_t room = -1;
            mtx_lock(manager->worker.sessionsLock);
            // Note that after unlocking sessionsLock, session_room can't be derefenrenced, only NULL-checked
            struct SessionRoom *session_room = hash_set_u32_get(manager->worker.sessions, cmd->kick.id);
            if (session_room != NULL)
                room = session_room->room;
            mtx_unlock(manager->worker.sessionsLock);

            if (session_room != NULL) {
                struct IdSession *id_session;
                struct RoomId *room_id = hash_set_u32_get(manager->rooms, room);
                if (room_id != NULL) {
                    struct Room *room = room_id->room;
                    id_session = hash_set_u32_get(room->sessions, cmd->kick.id);
                    if (id_session != NULL) {
                        struct Session *session = ((struct IdSession *)hash_set_u32_get(room->sessions, cmd->kick.id))->session;
                        free(cmd);
                        shutdown_session(session);
                    }
                }

                if (room_id == NULL || id_session == NULL) {
                    ssize_t thread = map_thread_coordinator_get(manager->worker.coordinator, room);
                    if (thread != -1) {
                        // The session is in another thread, forward the message to there
                        mtx_lock(&manager->worker.transportMuteces[thread]);
                        write(manager->worker.transportSinks[thread], &cmd, sizeof(struct WorkerCommand *));
                        mtx_unlock(&manager->worker.transportMuteces[thread]);
                    }
                    // Otherwise, the session has already disconnected
                }
            }
        }
        break;

        case WORKER_COMMAND_USER_COMMAND: {
            uint32_t room;
            mtx_lock(manager->worker.sessionsLock);
            struct SessionRoom *session_room = hash_set_u32_get(manager->worker.sessions, cmd->user.target);
            room = session_room->room;
            mtx_unlock(manager->worker.sessionsLock);

            if (session_room != NULL) {
                struct IdSession *id_session;
                struct RoomId *room_id = hash_set_u32_get(manager->rooms, room);
                if (room_id != NULL) {
                    struct Room *room = room_id->room;
                    id_session = hash_set_u32_get(room->sessions, cmd->user.id);
                    if (id_session != NULL) {
                        uint32_t id = cmd->user.id;
                        void *ctx = cmd->user.ctx;
                        cmd->user.res = 1;
                        sem_post(&cmd->user.sem);
                        struct Session *session = ((struct IdSession *)hash_set_u32_get(room->sessions, id))->session;
                        if (!setjmp(session->jmp))
                            manager->onClientCommand(session, ctx);
                    }
                }

                if (room_id == NULL || id_session == NULL) {
                    ssize_t thread = map_thread_coordinator_get(manager->worker.coordinator, room);
                    if (thread != -1) {
                        // The session is in another thread, forward the message to there
                        mtx_lock(&manager->worker.transportMuteces[thread]);
                        write(manager->worker.transportSinks[thread], &cmd, sizeof(struct WorkerCommand *));
                        mtx_unlock(&manager->worker.transportMuteces[thread]);
                    } else {
                        cmd->user.res = 0;
                        sem_post(&cmd->user.sem);
                    }
                }
            } else {
                cmd->user.res = 0;
                sem_post(&cmd->user.sem);
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

    uint8_t data[packet_len];

    evbuffer_remove(src, data, packet_len);
    decryption_context_decrypt(session->recieveContext, packet_len, data);

    printf("Received packet with opcode 0x%04hX from %hu\n", ((uint16_t *)data)[0], ((struct sockaddr_in *)&session->addr)->sin_port);
    for (uint16_t i = 0; i < packet_len; i++)
        printf("%02X ", data[i]);
    printf("\n\n");

    struct iovec vec[2] = {
        { &packet_len, sizeof(uint16_t) },
        { data, packet_len }
    };
    evbuffer_add_iovec(dst, vec, 2);

    return BEV_OK;
}

static enum bufferevent_filter_result output_filter(struct evbuffer *src,
                                                    struct evbuffer *dst,
                                                    ev_ssize_t size,
                                                    enum bufferevent_flush_mode mode,
                                                    void *ctx)
{
    struct Session *session = ctx;
    size_t len = evbuffer_get_length(src);
    uint16_t packet_len;
    evbuffer_copyout(src, &packet_len, sizeof(uint16_t));

    if (2 + packet_len > len)
        return BEV_NEED_MORE;

    evbuffer_drain(src, 2);

    uint8_t header[4];
    encryption_context_header(session->sendContext, packet_len, header);

    uint8_t data[packet_len];

    evbuffer_remove(src, data, packet_len);

    printf("Sending packet with opcode 0x%04hX to %hu\n",
           ((uint16_t *)data)[0],
           ((struct sockaddr_in *)&session->addr)->sin_port);
    for (uint16_t i = 0; i < packet_len; i++)
        printf("%02X ", data[i]);
    printf("\n\n");

    encryption_context_encrypt(session->sendContext, packet_len, data);

    struct iovec vec[2] = {
        { header, 4 },
        { data, packet_len }
    };
    evbuffer_add_iovec(dst, vec, 2);

    return BEV_OK;
}

static void on_pending_session_user_fd_ready(int fd, short what, void *ctx)
{
    struct Session *session = ctx;
    if (!setjmp(session->jmp)) {
        session->onResume(session, fd, libevent_to_poll(what));
    } else {
        destroy_pending_session(session);
    }
}

static void on_session_user_fd_ready(int fd, short what, void *ctx)
{
    struct Session *session = ctx;
    struct RoomManager *manager = session->supervisor;
    if (!setjmp(session->jmp)) {
        session->onResume(session, fd, libevent_to_poll(what));
        if (session->userEvent == NULL && session->timer != NULL) {
            struct timeval t;
            struct timeval now;
            evtimer_pending(session->timer, &t);
            event_base_gettimeofday_cached(manager->worker.base, &now);
            t.tv_sec -= now.tv_sec;
            t.tv_usec -= now.tv_usec;
            if (t.tv_usec < 0) {
                t.tv_usec += 1000000;
                t.tv_sec--;
            }

            evtimer_add(session->timer, &t);
            bufferevent_enable(session->event, EV_READ);
        } else if (session->timer == NULL) {
        }
    } else if (session->timer == NULL) {
        // We got a session_kick() and timer is NULL. meaning one of two things:
        // 1. While processsing the user event a disconnect request was issued
        //  either user-initiated or server-initiated (via another session_kick()).
        //  In this case we need to call the disconnect handler
        // 2. The disconnect handler posted a user event
        //  In this case we need to destroy the session
        if (!session->disconnecting)
            kick_common(session->supervisor, session, destroy_session);
        else
            destroy_session(session);
    }
}

static void on_user_fd_ready(int fd, short what, void *ctx)
{
    struct UserEvent *event = ctx;
    event->onResume(event->ctx, fd, libevent_to_poll(what));
    event_free(event->event);
    free(event);
}

static void on_session_read(struct bufferevent *event, void *ctx)
{
    struct Session *session = ctx;
    struct RoomManager *manager = session->supervisor;
    struct evbuffer *input = bufferevent_get_input(event);
    while (evbuffer_get_length(input) > 0) {
        uint16_t len;
        evbuffer_remove(input, &len, sizeof(uint16_t));
        uint8_t data[len];
        evbuffer_remove(input, data, len);
        if (!setjmp(session->jmp))
            manager->worker.onClientPacket(session, len, data);
        else
            break;
    }
}

static void on_pending_session_read(struct bufferevent *event, void *ctx)
{
    struct Session *session = ctx;
    struct ChannelServer *server = session->supervisor;
    struct evbuffer *input = bufferevent_get_input(event);
    uint16_t len;
    evbuffer_remove(input, &len, sizeof(uint16_t));
    uint8_t data[len];
    evbuffer_remove(input, data, len);
    if (!setjmp(session->jmp))
        server->worker.onClientPacket(session, len, data);
}

static void on_session_write(struct bufferevent *event, void *ctx)
{
    struct Session *session = ctx;
    if (evbuffer_get_length(bufferevent_get_output(event)) == 0) {
        if (bufferevent_get_underlying(event) != NULL) {
            void *temp = bufferevent_get_underlying(event);
            bufferevent_free(event);
            session->event = temp;
            if (evbuffer_get_length(bufferevent_get_output(session->event)) != 0)
                bufferevent_setcb(session->event, NULL, on_session_write, on_session_event, session);
            else
                do_transfer(session);
        } else {
            do_transfer(session);
        }
    }
}

static void on_session_timer_expired(int fd, short what, void *ctx)
{
    struct Session *session = ctx;
    struct RoomManager *manager = session->supervisor;

    struct timeval t = { .tv_sec = TIMER_FREQ };

    // We add the timer so that evtimer_pending() will return the correct
    // time when session_set_event() is called
    evtimer_add(session->timer, &t);

    if (!setjmp(session->jmp))
        manager->onClientTimer(session);
}

static void on_pending_session_event(struct bufferevent *event, short what, void *ctx)
{
    struct Session *session = ctx;
    struct Worker *worker =
        &((struct ChannelServer *)session->supervisor)->worker;

    if (what & BEV_EVENT_EOF || what & BEV_EVENT_ERROR)
        kick_common(worker, session, destroy_pending_session);
}

static void on_session_event(struct bufferevent *event, short what, void *ctx)
{
    struct Session *session = ctx;
    struct Worker *worker = &((struct RoomManager *)session->supervisor)->worker;

    if (what & BEV_EVENT_READING) {
        printf("Client %hu disconnected\n", ((struct sockaddr_in *)&session->addr)->sin_port);
        event_free(session->timer);
        session->timer = NULL;
        kick_common(worker, session, destroy_session);
    } else if (what & BEV_EVENT_WRITING) {
        shutdown_session(session);
    }
}

static int start_worker(void *ctx_)
{
    struct RoomManager *manager = ctx_;

    event_base_dispatch(manager->worker.base);

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

    session->id = 0;
    session->room = NULL;
    session->userEvent = NULL;
    session->writeEnable = false;
    session->targetRoom = -1;
    session->timer = NULL;
    session->disconnecting = false;

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

    room->sessions = hash_set_u32_create(sizeof(struct IdSession), offsetof(struct IdSession, id));
    if (room->sessions == NULL) {
        free(room->sessions);
        free(room);
        return NULL;
    }

    room->id = id;
    room->manager = manager;
    room->timerCapacity = 1;
    room->timerCount = 0;
    room->keepAlive = false;

    return room;
}

static void destroy_pending_session(struct Session *session)
{
    struct ChannelServer *server = session->supervisor;
    struct bufferevent *underlying = bufferevent_get_underlying(session->event);
    int fd = bufferevent_getfd(session->event);
    bufferevent_free(session->event);
    bufferevent_free(underlying);
    close(fd);
    encryption_context_destroy(session->sendContext);
    decryption_context_destroy(session->recieveContext);

    if (session->id != 0) {
        uint8_t data[5];
        data[0] = 1;
        memcpy(data + 1, &session->id, 4);
        struct LoggedOutNode *new = malloc(sizeof(struct LoggedOutNode));
        if (new == NULL)
            ; // TODO

        new->token = session->id;

        mtx_lock(server->worker.lock);
        new->next = *server->worker.head;
        *server->worker.head = new;
        if (*server->worker.connected)
            bufferevent_write(*server->worker.login, data, 5);
        mtx_unlock(server->worker.lock);
    }

    hash_set_addr_remove(server->sessions, (void *)&session->addr);

    free(session);
}

static void destroy_session(struct Session *session)
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

    mtx_lock(manager->worker.sessionsLock);
    hash_set_u32_remove(manager->worker.sessions, session->id);
    mtx_unlock(manager->worker.sessionsLock);

    if (session->id != 0) {
        uint8_t data[5];
        data[0] = 1;
        memcpy(data + 1, &session->id, 4);
        mtx_lock(manager->worker.lock);
        if (!*manager->worker.connected || bufferevent_write(*manager->worker.login, data, 5) == -1) {
            struct LoggedOutNode *new = malloc(sizeof(struct LoggedOutNode));
            if (new != NULL) {
                new->next = *manager->worker.head;
                new->token = session->id;
                *manager->worker.head = new;
            }
        }
        mtx_unlock(manager->worker.lock);
    }

    hash_set_u32_remove(room->sessions, session->id);
    if (room->timerCount == 0 && hash_set_u32_size(room->sessions) == 0 && !room->keepAlive)
        destroy_room(room);

    free(session);
}

static void kick_common(struct Worker *worker, struct Session *session, void (*destroy)(struct Session *session))
{
    if (session->userEvent == NULL) {
        if (!setjmp(session->jmp)) {
            session->disconnecting = true;
            worker->onClientDisconnect(session);
            bufferevent_disable(session->event, EV_READ);
        } else {
            destroy(session);
        }
    }
}

static void destroy_room(struct Room *room)
{
    struct RoomManager *manager = room->manager;

    for (size_t i = 0; i < room->timerCount; i++) {
        struct TimerHandle *timer = room->timers[i];
        event_free(timer->event);
        free(timer);
    }

    manager->onRoomDestroy(room);
    free(room->timers);
    hash_set_u32_remove(manager->rooms, room->id);
    hash_set_u32_destroy(room->sessions);
    map_thread_coordinator_unref(manager->worker.coordinator, room->id);
    free(room);
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


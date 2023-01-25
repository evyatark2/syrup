#include "server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <execinfo.h>

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/thread.h>

#include "../constants.h"
#include "../crypt.h"
#include "../hash-map.h"
#include "config.h"

#define MAPLE_VERSION 83

struct HeapNode {
    size_t threadIndex;
    size_t sessionCount;
};

struct LoginServer {
    /// The base event loop for connection listening
    struct event_base *loop;
    /// The connection listener
    struct evconnlistener *listener;
    /// Worker thread handles
    size_t threadCount;
    thrd_t *threads;

    /// Transport pipes used to transfer connected client to an available worker
    int *transportSinks;

    /// Minimum heap used to find the least busy worker
    mtx_t heapLock;
    struct HeapNode *minHeap;

    int commandSink;
    struct event *commandEvent;
};

static void on_command(int fd, short what, void *ctx);

static void on_session_connect(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *, int socklen, void *ctx);

enum SessionState {
    SESSION_STATE_CONNECTING,
    SESSION_STATE_CONNECTED,
    SESSION_STATE_DISCONNECTING,
    SESSION_STATE_KICKING,
};

struct Session {
    struct SessionContainer *container;
    struct Worker *worker;
    // Index inside the clients array of the worker
    /// The current state of the client
    enum SessionState state;
    /// The number of jobs this client is currently pending on
    struct bufferevent *event;
    struct EncryptionContext *sendContext;
    struct DecryptionContext *recieveContext;
    void *userData;
    OnResume *onResume;
    struct event *userEvent;
};

struct SessionList {
    size_t capacity;
    size_t count;
    struct Session *sessions;
};

static int session_list_init(struct SessionList *list);
static void session_list_destroy(struct SessionList *list);
static struct Session *session_list_allocate(struct SessionList *list);
static struct Session *session_list_get(struct SessionList *list, size_t index);
static void session_list_free(struct SessionList *list, struct Session *session);

// TODO: Maybe use a thread_local global instead of passing the struct between event callbacks
struct Worker {
    OnLog *onLog;
    OnClientCreate *onClientCreate;
    OnClientConnect *onClientConnect;
    OnClientPacket *onClientPacket;
    OnClientDisconnect *onClientDisconnect;
    OnClientDestroy *onClientDestroy;
    int transportSource;
    struct event_base *base;
    struct event *transportEvent;
    struct SessionList sessions;
    void *userData;
};

struct InitThreadUserContext {
    CreateUserContext *onInitContext;
};

struct FullThreadContext {
    struct Worker ctx;
    CreateUserContext *createContext;
    DestroyUserContext *destroyContext;
};

static int start_worker(void *ctx_);

static int init_session(struct Worker *server, struct Session *session, int fd);

static void on_user_fd_ready(int fd, short what, void *ctx);

// Closes fd in case of failure
static int finalize_session_connect(struct SessionContainer *container);
static void destroy_session(struct Session *session);
static bool kick_session(struct SessionContainer *container);

static short poll_to_libevent(int mask);
static int libevent_to_poll(short mask);

struct LoggedInCharacter {
    uint32_t token;
    uint32_t id;
    int fd;
};

struct Channel {
    struct sockaddr_storage addr;
    int socklen;
    struct bufferevent *event;
    mtx_t clientsMtx;
    struct HashSetU32 *clients;
    mtx_t mtx;
    bool connected;
    int attempts;
    OnClientLeave *onClientLeave;
};

struct Channel CHANNELS[WORLD_COUNT][CHANNEL_COUNT];

static void on_channel_read(struct bufferevent *bev, void *ctx);
static void on_channel_event(struct bufferevent *bev, short what, void *ctx);

struct LoginServer *login_server_create(OnLog *on_log, CreateUserContext *create_user_context, DestroyUserContext destroy_user_ctx, OnClientCreate *on_client_create, OnClientConnect *on_client_connect, OnClientPacket *on_client_packet, OnClientDisconnect *on_client_disconnect, OnClientDestroy *on_client_destroy, OnClientLeave *on_client_leave)
{
    struct LoginServer *server = malloc(sizeof(struct LoginServer));
    if (server == NULL)
        return NULL;

    evthread_use_pthreads();

    server->loop = event_base_new();
    if (server->loop == NULL)
        goto free_server;

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(8484),
    };

    server->listener = evconnlistener_new_bind(server->loop, on_session_connect, server, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, (const struct sockaddr *)&addr, sizeof(struct sockaddr_in));
    if (server->listener == NULL)
        goto free_loop;

    for (uint8_t i = 0; i < LOGIN_CONFIG.worldCount; i++) {
        for (uint8_t j = 0; j < LOGIN_CONFIG.worlds[i].channelCount; j++) {
            struct Channel *channel = &CHANNELS[i][j];

            if (mtx_init(&channel->mtx, mtx_plain) != thrd_success)
                return NULL;

            if (mtx_init(&channel->clientsMtx, mtx_plain) != thrd_success) {
                mtx_destroy(&channel->mtx);
                return NULL;
            }

            int domain;
            struct in_addr addr4;
            struct in6_addr addr6;
            if (evutil_inet_pton(AF_INET, LOGIN_CONFIG.worlds[i].channels[j].host, &addr4) == 1)
                domain = PF_INET;
            else if (evutil_inet_pton(AF_INET6, LOGIN_CONFIG.worlds[i].channels[j].host, &addr6) == 1)
                domain = PF_INET6;
            else
                domain = PF_UNIX;

            channel->event = bufferevent_socket_new(server->loop, -1, BEV_OPT_THREADSAFE);
            switch (domain) {
            case AF_INET: {
                struct sockaddr_in addr = {
                    .sin_family = AF_INET,
                    .sin_addr = addr4,
                    .sin_port = htons(strtol(strchr(LOGIN_CONFIG.worlds[i].channels[j].host, ':') + 1, NULL, 10)),
                };
                memcpy(&channel->addr, &addr, sizeof(struct sockaddr_un));
                channel->socklen = sizeof(struct sockaddr_in);
            }
                break;
            case AF_INET6: {
                struct sockaddr_in6 addr = {
                    .sin6_family = AF_INET,
                    .sin6_addr = addr6,
                    .sin6_port = htons(strtol(strchr(LOGIN_CONFIG.worlds[i].channels[j].host, ']') + 2, NULL, 10)),
                };
                memcpy(&channel->addr, &addr, sizeof(struct sockaddr_in6));
                channel->socklen = sizeof(struct sockaddr_in6);
            }
            break;
            case AF_UNIX: {
                struct sockaddr_un addr = {
                    .sun_family = AF_UNIX,
                };
                strcpy(addr.sun_path, LOGIN_CONFIG.worlds[i].channels[j].host);
                memcpy(&channel->addr, &addr, sizeof(struct sockaddr_in6));
                channel->socklen = sizeof(struct sockaddr_un);
            }
            break;
            }

            if (bufferevent_socket_connect(channel->event, (void *)&channel->addr, channel->socklen) == -1) {
                bufferevent_free(channel->event);
                return NULL;
            }

            channel->clients = NULL;
            channel->connected = false;
            channel->attempts = 0;

            bufferevent_setcb(channel->event, on_channel_read, NULL, on_channel_event, channel);
            bufferevent_enable(channel->event, EV_READ | EV_WRITE);

            struct timeval time = {
                .tv_sec = 10,
                .tv_usec = 0
            };
            bufferevent_set_timeouts(channel->event, &time, NULL);

            channel->onClientLeave = on_client_leave;
        }
    }

    // Start the watch dog thread
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    // Start the worker threads
    server->transportSinks = malloc(nproc * sizeof(int));
    if (server->transportSinks == NULL)
        goto free_listener;

    int pipefds[2];
    if (pipe(pipefds) == -1)
        goto free_sockets;

    server->commandSink = pipefds[1];
    server->commandEvent = event_new(server->loop, pipefds[0], EV_READ | EV_PERSIST, on_command, server);
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
        int pipefds[2];
        if (pipe(pipefds) == -1)
            goto exit_threads;

        server->transportSinks[server->threadCount] = pipefds[1];

        struct FullThreadContext *ctx = malloc(sizeof(struct FullThreadContext));
        if (ctx == NULL) {
            close(pipefds[0]);
            close(pipefds[1]);
            goto exit_threads;
        }

        ctx->ctx.onLog = on_log;
        ctx->ctx.onClientCreate = on_client_create;
        ctx->ctx.onClientConnect = on_client_connect;
        ctx->ctx.onClientPacket = on_client_packet;
        ctx->ctx.onClientDisconnect = on_client_disconnect;
        ctx->ctx.onClientDestroy = on_client_destroy;
        ctx->ctx.transportSource = pipefds[0];
        ctx->createContext = create_user_context;
        ctx->destroyContext = destroy_user_ctx;

        if (thrd_create(server->threads + server->threadCount, start_worker, ctx) != thrd_success) {
            free(ctx);
            close(pipefds[0]);
            close(pipefds[1]);
            goto exit_threads;
        }
    }

    return server;

exit_threads:
    for (size_t i = 0; i < server->threadCount; i++) {
        close(server->transportSinks[i]);
        thrd_join(server->threads[i], NULL);
    }

    free(server->threads);

free_command_event:
    close(event_get_fd(server->commandEvent));
    event_free(server->commandEvent);

close_command:
    close(server->commandSink);

free_sockets:
    free(server->transportSinks);

free_listener:
    evconnlistener_free(server->listener);

free_loop:
    event_base_free(server->loop);

free_server:
    free(server);
    return NULL;
}

void login_server_destroy(struct LoginServer *server)
{
    for (uint8_t i = 0; i < LOGIN_CONFIG.worldCount; i++) {
        for (uint8_t j = 0; j < LOGIN_CONFIG.worlds[i].channelCount; j++) {
            mtx_destroy(&CHANNELS[i][j].clientsMtx);
            mtx_destroy(&CHANNELS[i][j].mtx);
            hash_set_u32_destroy(CHANNELS[i][j].clients);
        }
    }
    free(server->threads);
    free(server->transportSinks);
    event_base_free(server->loop);
    free(server);
}

enum ResponderResult login_server_start(struct LoginServer *server)
{
    if (event_base_dispatch(server->loop) == -1)
        return -1;

    for (size_t i = 0; i < server->threadCount; i++)
        close(server->transportSinks[i]);

    for (size_t i = 0; i < server->threadCount; i++)
        thrd_join(server->threads[i], NULL);

    return RESPONDER_RESULT_SUCCESS;
}

void login_server_stop(struct LoginServer *server)
{
    close(server->commandSink);
}

int assign_channel(uint32_t id, uint8_t world, uint8_t channel, uint32_t *token)
{
    int fd = eventfd(0, 0);
    if (fd == -1)
        return -1;

    struct LoggedInCharacter pending = {
        .id = id,
        .fd = fd
    };

    mtx_lock(&CHANNELS[world][channel].mtx);

    if (!CHANNELS[world][channel].connected) {
        mtx_unlock(&CHANNELS[world][channel].mtx);
        close(fd);
        return -1;
    }

    mtx_lock(&CHANNELS[world][channel].clientsMtx);
    do {
        pending.token = rand() % 32768 << 16 | rand() % 32768;
    // 0 is an invalid token
    } while (pending.token == 0 || hash_set_u32_get(CHANNELS[world][channel].clients, pending.token) != NULL);
    if (hash_set_u32_insert(CHANNELS[world][channel].clients, &pending) == -1) {
        mtx_unlock(&CHANNELS[world][channel].clientsMtx);
        mtx_unlock(&CHANNELS[world][channel].mtx);
        close(fd);
        return -1;
    }

    mtx_unlock(&CHANNELS[world][channel].clientsMtx);

    bufferevent_write(CHANNELS[world][channel].event, (uint32_t[]) { pending.token, pending.id }, 8);
    mtx_unlock(&CHANNELS[world][channel].mtx);
    *token = pending.token;
    return fd;
}

int session_get_event_disposition(struct Session *session)
{
    return libevent_to_poll(event_get_events(session->userEvent));
}

int session_set_event(struct Session *session, int status, int fd, OnResume *on_resume)
{
    void *temp = event_new(session->worker->base, fd, poll_to_libevent(status), on_user_fd_ready, session->container);
    if (temp == NULL)
        return -1;

    if (event_add(temp, NULL) == -1) {
        event_free(temp);
        return -1;
    }

    if (session->userEvent != NULL)
        event_free(session->userEvent);

    session->userEvent = temp;
    session->onResume = on_resume;

    return 0;
}

void session_write(struct Session *client, size_t len, uint8_t *packet)
{
    if (len == 0)
        return;

    printf("Sending packet with opcode %hd\n", ((uint16_t *)packet)[0]);
    for (uint16_t i = 0; i < len; i++)
        printf("%02X ", packet[i]);
    printf("\n\n");

    uint16_t packet_len = len;
    bufferevent_write(client->event, &packet_len, 2);
    bufferevent_write(client->event, packet, len);
}

enum CommandType {
    COMMAND_TYPE_KICK
};

struct Command {
    enum CommandType type;
};

static void on_command(int fd, short what, void *ctx)
{
    struct LoginServer *server = ctx;
    struct Command command;
    int status = read(fd, &command, sizeof(struct Command));
    if (status == 0) {
        close(event_get_fd(server->commandEvent));
        event_free(server->commandEvent);
        evconnlistener_free(server->listener);
        for (uint8_t i = 0; i < LOGIN_CONFIG.worldCount; i++) {
            for (uint8_t j = 0; j < LOGIN_CONFIG.worlds[i].channelCount; j++)
                bufferevent_free(CHANNELS[i][j].event);
        }
    }
}

static void do_leave(void *data, void *ctx);

static void on_channel_read(struct bufferevent *bev, void *ctx)
{
    struct Channel *channel = ctx;
    if (!channel->connected) {
        uint8_t first;
        evbuffer_remove(bufferevent_get_input(bev), &first, 1);
        if (first == 0) {
            // We are the first one to connect to the channel,
            // This means that either:
            // 1) This is the first connection that this login server makes to this channel server, in which case we need to create the pending list
            // 2) The channel server was restarted, in which case we need to re-create the pending list
            mtx_lock(&channel->clientsMtx);
            if (channel->clients != NULL)
                hash_set_u32_foreach(channel->clients, do_leave, channel);

            mtx_unlock(&channel->clientsMtx);
            hash_set_u32_destroy(channel->clients);
            channel->clients = hash_set_u32_create(sizeof(struct LoggedInCharacter), offsetof(struct LoggedInCharacter, token));
        } else {
            if (channel->clients == NULL) {
                // We are not the first one to connect to the channel, and we don't have a logged-in list
                // This means that either:
                // 1) The connection to the channel server was lost and we waited long enough (30 sec) to reconnect
                //    that we decided to drop the logged-in list in order to let the connected accounts re-login
                // 2) The login server was restarted and the channel now received the second connection
                // In both cases we let the channel server know to kick all clients
                channel->clients = hash_set_u32_create(sizeof(struct LoggedInCharacter), offsetof(struct LoggedInCharacter, token));
                uint8_t one = 1;
                bufferevent_write(bev, &one, 1);
            } else {
                // We are not the first one to connect to the channel, and we have a logged-in list
                // This means that the connection to the channel server was lost and we reconnected in time
                uint8_t zero = 0;
                bufferevent_write(bev, &zero, 1);
            }
        }
        mtx_lock(&channel->mtx);
        channel->connected = true;
        mtx_unlock(&channel->mtx);
    } else {
        if (evbuffer_get_length(bufferevent_get_input(bev)) < 5)
            return;

        uint8_t action;
        evbuffer_remove(bufferevent_get_input(bev), &action, 1);
        uint32_t token;
        evbuffer_remove(bufferevent_get_input(bev), &token, 4);
        if (action == 0) {
            mtx_lock(&channel->clientsMtx);
            struct LoggedInCharacter *chr = hash_set_u32_get(channel->clients, token);
            int fd = chr->fd;
            chr->fd = -1;
            mtx_unlock(&channel->clientsMtx);
            uint64_t one = 1;
            write(fd, &one, 8);
        } else if (action == 1) { // Client disconnects
            channel->onClientLeave(token);
        }
    }
}

static void on_channel_event(struct bufferevent *bev, short what, void *ctx)
{
    struct Channel *channel = ctx;
    if (what & BEV_EVENT_CONNECTED) {
        channel->attempts = 0;
        bufferevent_set_timeouts(bev, NULL, NULL);
    } else if (what & BEV_EVENT_TIMEOUT) {
        bufferevent_socket_connect(bev, (void *)&channel->addr, channel->socklen);

        if (channel->attempts < 3) {
            channel->attempts++;
            if (channel->attempts == 3) {
                hash_set_u32_foreach(channel->clients, do_leave, channel);
                hash_set_u32_destroy(channel->clients);
                channel->clients = NULL;
            }
        }

        bufferevent_enable(bev, EV_READ);
    } else if (what & BEV_EVENT_EOF || what & BEV_EVENT_ERROR) {
        struct bufferevent *new = bufferevent_socket_new(bufferevent_get_base(bev), -1, BEV_OPT_THREADSAFE);

        bufferevent_socket_connect(new, (void *)&channel->addr, channel->socklen);

        channel->attempts = 0;

        bufferevent_setcb(new, on_channel_read, NULL, on_channel_event, channel);
        bufferevent_enable(new, EV_READ | EV_WRITE);
        struct timeval time = {
            .tv_sec = 10,
            .tv_usec = 0
        };
        bufferevent_set_timeouts(new, &time, NULL);

        mtx_lock(&channel->mtx);
        channel->connected = false;
        bufferevent_free(channel->event);
        channel->event = new;
        mtx_unlock(&channel->mtx);
    }
}

struct FdAndAddress {
    int fd;
    struct sockaddr_storage address;
};

static void on_session_connect(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *addr, int socklen, void *ctx)
{
    struct LoginServer *server = ctx;
    //size_t thread = server->minHeap[0].threadIndex;
    //server->minHeap[0] = server->minHeap[server->threadCount - 1];
    //size_t i = 0;
    //while (i < server->threadCount) {
    //    //if (server->minHeap[i]
    //}

    struct FdAndAddress fd_and_addr = {
        .fd = fd
    };
    memcpy(&fd_and_addr.address, addr, socklen);
    write(server->transportSinks[0], &fd_and_addr, sizeof(struct FdAndAddress));
}

static enum bufferevent_filter_result input_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx_);
static enum bufferevent_filter_result output_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx_);
static void on_session_read(struct bufferevent *event, void *ctx);
static void on_session_event(struct bufferevent *event, short what, void *ctx);

static void on_session_thread_assign(int fd, short what, void *ctx_)
{
    struct Worker *worker = ctx_;
    ssize_t status;
    struct FdAndAddress fd_and_addr;

    if ((status = read(fd, &fd_and_addr, sizeof(struct FdAndAddress))) == 0) {
        int fd = event_get_fd(worker->transportEvent);
        event_free(worker->transportEvent);
        close(fd);
        worker->transportEvent = NULL;
        for (size_t i = 0; i < worker->sessions.count; i++) {
            struct Session *session = session_list_get(&worker->sessions, i);
            if (session->state != SESSION_STATE_DISCONNECTING) {
                if (session->userEvent == NULL) {
                    if (kick_session(session->container)) {
                        i--;
                    }
                } else {
                    session->state = SESSION_STATE_KICKING;
                }
            }
        }

    } else if (status != -1) {
        struct Session *session = session_list_allocate(&worker->sessions);
        if (session == NULL) {
            close(fd_and_addr.fd);
            return;
        }

        if (init_session(worker, session, fd_and_addr.fd) == -1) {
            close(fd_and_addr.fd);
            return;
        }

        session->container = worker->onClientCreate(worker->userData);
        if (session->container == NULL) {
            destroy_session(session);
            return;
        }

        session->container->session = session;

        char ip[INET_ADDRSTRLEN];
        evutil_inet_ntop(AF_INET, &((struct sockaddr_in *)&fd_and_addr.address)->sin_addr, ip, INET_ADDRSTRLEN);
        worker->onLog(LOG_OUT, "New connection from %s:%hu\n", ip, ntohs(((struct sockaddr_in *)&fd_and_addr.address)->sin_port));
        int status = worker->onClientConnect(session->container, (struct sockaddr *)&fd_and_addr.address);
        if (status == 0 && session->userEvent == NULL) {
            finalize_session_connect(session->container);
        } else if (status < 0) {
            worker->onClientDestroy(session->container);
            destroy_session(session);
        }
    } else {
        worker->onLog(LOG_ERR, "Reading a new client fd failed with %d: %s. Client's file descriptor leaked.\n", errno, strerror(errno));
    }
}

static enum bufferevent_filter_result input_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx)
{
    struct SessionContainer *container = ctx;
    struct Session *session = container->session;
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
    evbuffer_add(dst, data, packet_len);

    return BEV_OK;
}

static enum bufferevent_filter_result output_filter(struct evbuffer *src, struct evbuffer *dst, ev_ssize_t size, enum bufferevent_flush_mode mode, void *ctx)
{
    struct SessionContainer *container = ctx;
    struct Session *session = container->session;
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

    encryption_context_encrypt(session->sendContext, packet_len, data);

    evbuffer_add(dst, header, 4);
    evbuffer_add(dst, data, packet_len);

    return BEV_OK;
}

static void on_user_fd_ready(int fd, short what, void *ctx_)
{
    struct SessionContainer *container = ctx_;
    struct Session *session = container->session;
    int status = session->onResume(container, fd, libevent_to_poll(what));
    if (status <= 0) {
        event_free(session->userEvent);
        session->userEvent = NULL;
        if (status == 0) {
            if (session->state == SESSION_STATE_CONNECTING && finalize_session_connect(container) == -1) {
                kick_session(container);
            } else if (session->state == SESSION_STATE_KICKING) {
                kick_session(container);
            } else if (session->state == SESSION_STATE_DISCONNECTING) {
                session->worker->onClientDestroy(container);
                destroy_session(session);
            } else {
                bufferevent_enable(session->event, EV_READ);
            }
        } else {
            kick_session(container);
        }
    }
}

static void on_session_read(struct bufferevent *event, void *ctx)
{
    struct SessionContainer *container = ctx;
    struct Session *session = container->session;
    struct Worker *worker = session->worker;
    struct evbuffer *input = bufferevent_get_input(event);
    // TODO: Maybe there is no one-to-one correspondence between on_session_read and input_filter
    // meaning the input buffer can contain multiple packets
    size_t len = evbuffer_get_length(input);
    uint8_t data[len];
    evbuffer_remove(input, data, len);
    int status = worker->onClientPacket(container, len, data);
    if (status == 0 && session->userEvent != NULL) {
        bufferevent_disable(event, EV_READ);
    } else if (status < 0) {
        kick_session(ctx);
    }
}

static void on_session_event(struct bufferevent *event, short what, void *ctx_)
{
    struct SessionContainer *container = ctx_;
    struct Session *session = container->session;
    struct Worker *worker = session->worker;
    if (what & BEV_EVENT_EOF || ((what & BEV_EVENT_ERROR) && (what & BEV_EVENT_READING))) {
        worker->onLog(LOG_OUT, "Client disconnected\n");
        worker->onClientDisconnect(container);
        if (session->userEvent == NULL) {
            session->worker->onClientDestroy(container);
            destroy_session(session);
        } else {
            session->state = SESSION_STATE_DISCONNECTING;
        }
    }
    // The most likely write error, EPIPE, doesn't need any special handling
    // as eventually on_session_event will be called with BEV_EVENT_EOF
}

static int start_worker(void *ctx_)
{
    struct Worker ctx = ((struct FullThreadContext *)ctx_)->ctx;
    DestroyUserContext *destroy_ctx = ((struct FullThreadContext *)ctx_)->destroyContext;

    ctx.userData = ((struct FullThreadContext *)ctx_)->createContext();

    free(ctx_);

    ctx.base = event_base_new();
    if (ctx.base == NULL)
        ;

    if (session_list_init(&ctx.sessions) == -1)
        ;

    ctx.transportEvent = event_new(ctx.base, ctx.transportSource, EV_READ | EV_PERSIST, on_session_thread_assign, &ctx);
    event_add(ctx.transportEvent, NULL);

    event_base_dispatch(ctx.base);

    session_list_destroy(&ctx.sessions);
    event_base_free(ctx.base);
    destroy_ctx(ctx.userData);

    return 0;
}

static int init_session(struct Worker *server, struct Session *session, int fd)
{
    session->worker = server;
    uint8_t iv[4] = { 0 };
    session->sendContext = encryption_context_new(iv, ~MAPLE_VERSION);
    if (session->sendContext == NULL)
        goto free_slot;
    session->recieveContext = decryption_context_new(iv);
    if (session->recieveContext == NULL)
        goto destroy_send_context;
    session->event = bufferevent_socket_new(server->base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (session->event == NULL)
        goto destroy_recieve_context;

    bufferevent_enable(session->event, EV_WRITE);

    session->userEvent = NULL;

    return 0;

destroy_recieve_context:
    decryption_context_destroy(session->recieveContext);
destroy_send_context:
    encryption_context_destroy(session->sendContext);
free_slot:
    session_list_free(&server->sessions, session);
    return -1;
}

static int finalize_session_connect(struct SessionContainer *container)
{
    struct Session *session = container->session;
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
    bufferevent_write(session->event, data, 16);

    void *temp = bufferevent_filter_new(session->event, input_filter, output_filter, BEV_OPT_CLOSE_ON_FREE, NULL, container);
    if (temp == NULL)
        return -1;

    session->event = temp;

    bufferevent_setcb(session->event, on_session_read, NULL, on_session_event, container);
    bufferevent_enable(session->event, EV_READ | EV_WRITE);

    return 0;
}

static void destroy_session(struct Session *session)
{
    bufferevent_free(session->event);
    encryption_context_destroy(session->sendContext);
    decryption_context_destroy(session->recieveContext);
    session_list_free(&session->worker->sessions, session);
}

static bool kick_session(struct SessionContainer *container)
{
    struct Session *session = container->session;
    struct Worker *worker = session->worker;
    worker->onClientDisconnect(container);
    if (session->userEvent == NULL) {
        worker->onClientDestroy(container);
        destroy_session(session);
        return true;
    }

    session->state = SESSION_STATE_DISCONNECTING;
    return false;
}

static void do_leave(void *data, void *ctx)
{
    struct LoggedInCharacter *chr = data;
    struct Channel *channel = ctx;
    if (chr->fd == -1) {
        channel->onClientLeave(chr->token);
    } else {
        uint64_t two = 2;
        write(chr->fd, &two, 8);
    }
}

static int session_list_init(struct SessionList *list)
{
    list->sessions = malloc(sizeof(struct Session));
    if (list->sessions == NULL)
        return -1;

    list->capacity = 1;
    list->count = 0;

    return 0;
}

static void session_list_destroy(struct SessionList *list)
{
    free(list->sessions);
}

static struct Session *session_list_allocate(struct SessionList *list)
{
    if (list->count == list->capacity) {
        void *temp = realloc(list->sessions, (list->capacity * 2) * sizeof(struct Session));
        if (temp == NULL)
            return NULL;

        if (temp != list->sessions) {
            list->sessions = temp;
            for (size_t i = 0; i < list->count; i++)
                list->sessions[i].container->session = &list->sessions[i];
        } else {
            list->sessions = temp;
        }

        list->capacity *= 2;
    }

    list->count++;
    return &list->sessions[list->count - 1];
}

static struct Session *session_list_get(struct SessionList *list, size_t index)
{
    if (index >= list->count)
        return NULL;
    return &list->sessions[index];
}

static void session_list_free(struct SessionList *list, struct Session *session)
{
    size_t index = session - list->sessions;
    list->sessions[index] = list->sessions[list->count - 1];
    if (index != list->count - 1)
        list->sessions[index].container->session = &list->sessions[index];
    list->count--;
    if (list->count < list->capacity / 4) {
        void *temp = realloc(list->sessions, (list->capacity / 2) * sizeof(struct Session));
        if (temp != list->sessions) {
            list->sessions = temp;
            for (size_t i = 0; i < list->count; i++)
                list->sessions[i].container->session = &list->sessions[i];
        } else {
            list->sessions = temp;
        }
    }
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
    if (ret & EV_READ)
        ret |= POLLIN;

    if (mask & EV_WRITE)
        ret |= POLLOUT;

    return ret;
}


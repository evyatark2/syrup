#ifndef CHANNEL_SERVER_H
#define CHANNEL_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/socket.h>
#include <poll.h>

enum LogType {
    LOG_OUT,
    LOG_ERR
};

enum ResponderResult {
    RESPONDER_RESULT_ERROR = -1,
    RESPONDER_RESULT_SUCCESS,
};

struct ChannelServer;
struct Session;

struct Room;

struct Event;

typedef void OnLog(enum LogType type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

typedef void VoidFunc(void);

// If event is not NULL then it represents a new event to listen on; In this case, status and room are ignored.
// If event is NULL then status represents the poll flags to continue listening on the current event
// or a 0 in case of success or a negative value in case of failure
// If event is NULL and status is 0 and room is not -1 then a room transfer will take place to the room with ID \p room.
// If OnResume was invoked as a result of returning an event from OnClientConnect then room is ignored
// and a room change cannot happen.
struct OnResumeResult {
    int status;
    uint32_t room;
};
typedef struct OnResumeResult OnResume(struct Session *session, int fd, int status);

struct OnConnectResult {
    int status;
    OnResume *onResume;
};

typedef int OnRoomResume(struct Room *room, int fd, int status);

typedef int OnClientConnect(struct Session *session, void *global_ctx, void *thread_ctx, struct sockaddr *addr);

typedef void OnClientDisconnect(struct Session *session);
typedef bool OnClientJoin(struct Session *session, void *thread_ctx);

struct OnPacketResult {
    int status;
    uint32_t room;
};

typedef struct OnPacketResult OnClientPacket(struct Session *session, size_t size, uint8_t *packet);

typedef int OnRoomCreate(struct Room *room, void *thread_ctx);
typedef void OnRoomDestroy(struct Room *room);

typedef int OnDatabaseResult(void);

typedef void *CreateUserContext(void);
typedef void DestroyUserContext(void *ctx);

typedef void OnLoginPacket(size_t len, void *data);

struct ChannelServer *channel_server_create(uint16_t port, OnLog *on_log, const char *host, CreateUserContext *create_user_context, DestroyUserContext destroy_user_ctx, OnClientConnect *on_client_connect, OnClientDisconnect *on_client_disconnect, OnClientJoin *on_client_join, OnClientPacket *on_pending_client_packet, OnClientPacket *on_client_packet, OnRoomCreate *on_room_create, OnRoomDestroy *on_room_destroy, void *global_ctx, size_t event_count);
void channel_server_destroy(struct ChannelServer *server);
struct Event *channel_server_get_event(struct ChannelServer *server, size_t event);
enum ResponderResult channel_server_start(struct ChannelServer *server);
void channel_server_stop(struct ChannelServer *server);

bool session_assign_token(struct Session *session, uint32_t token, uint32_t *id);
void session_write(struct Session *session, size_t len, uint8_t *packet);
void session_set_context(struct Session *session, void *ctx);
void *session_get_context(struct Session *session);
int session_get_event_disposition(struct Session *session);
int session_get_event_fd(struct Session *session);
int session_set_event(struct Session *session, int status, int fd, OnResume *on_resume);
void session_close_event(struct Session *session);
struct Room *session_get_room(struct Session *session);
void session_broadcast_to_room(struct Session *session, size_t len, uint8_t *packet);
void session_foreach_in_room(struct Session *session, void (*f)(struct Session *src, struct Session *dst, void *ctx), void *ctx);
void session_enable_write(struct Session *session);

uint32_t room_get_id(struct Room *room);
int room_set_event(struct Room *room, int fd, int status, OnRoomResume *on_resume);
void room_close_event(struct Room *room);
void room_set_context(struct Room *room, void *ctx);
void *room_get_context(struct Room *room);
void room_broadcast(struct Room *room, size_t len, uint8_t *packet);
void room_foreach(struct Room *room, void (*f)(struct Session *src, struct Session *dst, void *ctx), void *ctx_);
/// Enable the keep alive trigger
void room_keep_alive(struct Room *room);
/// Disable the keep alive trigger
void room_break_off(struct Room *room);

void event_set_property(struct Event *event, uint32_t property, int32_t value);
int32_t event_get_property(struct Event *event, uint32_t property);
uint32_t event_add_listener(struct Event *event, uint32_t property, void (*f)(void *), void *ctx);
void event_remove_listener(struct Event *event, uint32_t property, uint32_t listener_id);
void event_schedule(struct Event *e, void f(struct Event *, void *), void *ctx, const struct timespec *tm);

struct TimerHandle;
struct TimerHandle *room_add_timer(struct Room *room, uint64_t msec, void (*f)(struct Room *, struct TimerHandle *), void *data, bool keep_alive);
struct Room *timer_get_room(struct TimerHandle *handle);
void timer_set_data(struct TimerHandle *handle, void *data);
void *timer_get_data(struct TimerHandle *handle);
void room_stop_timer(struct TimerHandle *handle);

#endif


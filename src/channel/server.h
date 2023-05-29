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

struct UserEvent;

typedef void OnLog(enum LogType type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

typedef void VoidFunc(void);

typedef void OnResumeEvent(void *ctx, int fd, int status);

typedef void OnResume(struct Session *session, int fd, int status);

typedef int OnRoomResume(struct Room *room, int fd, int status);

typedef void OnClientConnect(struct Session *session, void *global_ctx, void *thread_ctx, struct sockaddr *addr);

typedef void OnClientDisconnect(struct Session *session);
typedef void OnClientJoin(struct Session *session, void *thread_ctx);

typedef void OnClientPacket(struct Session *session, size_t size, uint8_t *packet);

typedef int OnRoomCreate(struct Room *room, void *thread_ctx);
typedef void OnRoomDestroy(struct Room *room);

typedef void OnClientTimer(struct Session *session);
typedef void OnClientCommandResult(struct Session *session, void *cmd, bool sent);
typedef void OnClientCommand(struct Session *session, void *cmd);

typedef void *CreateUserContext(void);
typedef void DestroyUserContext(void *ctx);

struct ChannelServer *channel_server_create(uint16_t port, OnLog *on_log, const char *host, CreateUserContext *create_user_context, DestroyUserContext destroy_user_ctx, OnClientConnect *on_client_connect, OnClientDisconnect *on_client_disconnect, OnClientJoin *on_client_join, OnClientPacket *on_pending_client_packet, OnClientPacket *on_client_packet, OnRoomCreate *on_room_create, OnRoomDestroy *on_room_destroy, OnClientCommand on_client_command, OnClientTimer on_client_timer, void *global_ctx, size_t event_count);
void channel_server_destroy(struct ChannelServer *server);
struct Event *channel_server_get_event(struct ChannelServer *server, size_t event);
enum ResponderResult channel_server_start(struct ChannelServer *server);
void channel_server_stop(struct ChannelServer *server);

bool session_accept(struct Session *session);
const struct sockaddr *session_get_addr(struct Session *session);
bool session_assign_id(struct Session *session, uint32_t id);
void session_change_room(struct Session *session, uint32_t id);
void session_kick(struct Session *session);
void session_write(struct Session *session, size_t len, uint8_t *packet);
void session_set_context(struct Session *session, void *ctx);
void *session_get_context(struct Session *session);
int session_get_event_disposition(struct Session *session);
int session_get_event_fd(struct Session *session);
int session_set_event(struct Session *session, int status, int fd, OnResume *on_resume);
struct UserEvent *session_add_event(struct Session *session, int status, int fd, OnResumeEvent *on_resume, void *ctx);
int session_close_event(struct Session *session);
struct Room *session_get_room(struct Session *session);
void session_broadcast_to_room(struct Session *session, size_t len, uint8_t *packet);
void session_foreach_in_room(struct Session *session, void (*f)(struct Session *src, struct Session *dst, void *ctx), void *ctx);
void session_enable_write(struct Session *session);
bool session_send_command(struct Session *session, uint32_t target, void *command);

void *room_get_base(struct Room *room);
uint32_t room_get_id(struct Room *room);
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
uint32_t event_add_listener(struct Event *event, void *base, uint32_t property, void (*f)(void *), void *ctx);
void event_remove_listener(struct Event *event, uint32_t property, uint32_t listener_id);
void event_schedule(struct Event *e, void f(struct Event *, void *), void *ctx, const struct timespec *tm);

struct TimerHandle;
struct TimerHandle *room_add_timer(struct Room *room, uint64_t msec, void (*f)(struct Room *, struct TimerHandle *), void *data);
void room_stop_timer(struct TimerHandle *timer);
struct Room *timer_get_room(struct TimerHandle *handle);
void timer_set_data(struct TimerHandle *handle, void *data);
void *timer_get_data(struct TimerHandle *handle);

struct UserEvent *user_event_add_event(struct UserEvent *ev, int status, int fd, OnResumeEvent *on_resume, void *ctx);

#endif


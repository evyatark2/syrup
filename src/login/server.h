#ifndef LOGIN_SERVER_H
#define LOGIN_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include <sys/socket.h>
#include <poll.h>

#include "../database.h"

enum LogType {
    LOG_OUT,
    LOG_ERR
};

enum ResponderResult {
    RESPONDER_RESULT_SUCCESS,
};

struct LoginServer;

struct Session;
struct SessionContainer {
    struct Session *session;
};

typedef void OnLog(enum LogType type, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

typedef void VoidFunc(void);

// Positive value - Continue listening on the event that was last set by session_set_event() for further OnResume() invocations
// Zero - The action has been completed, free up the current event
// Negative value - There was an error, the event will be free'd and OnClientDisconnect() will be called as soon as possible
// When resuming an OnClientDisconnect() returning a zero or a negative value have the same effect, they free up the session
// and OnClientDisconnect() isn't called again.
typedef int OnResume(struct SessionContainer *session, int fd, int status);

typedef struct SessionContainer *OnClientCreate(void *thread_ctx);

typedef void OnClientDestroy(struct SessionContainer *session);

// Zero - If session_set_event() was called during the invocation then it will be added to the pending event list
//      otherwise, the action is done and the connection will be finalized
// Negative value - There was an error, the event will be free'd and OnClientDestroy() will be called as soon as possible
typedef int OnClientConnect(struct SessionContainer *session, struct sockaddr *addr);

// If session_set_event() wasn't called then OnClientDestroy() will be called as soon as possible,
// otherwise, an OnResume() will be called
typedef void OnClientDisconnect(struct SessionContainer *session);

// Zero - If session_set_event() was called during the invocation then it will be added to the pending event list
//      otherwise, the action is done
// Negative value - There was an error, the event will be free'd and OnClientDisconnect() will be called as soon as possible
typedef int OnClientPacket(struct SessionContainer *session, size_t size, uint8_t *packet);

typedef void OnClientLeave(uint32_t token);

typedef void *CreateUserContext();
typedef void DestroyUserContext(void *ctx);

struct LoginServer *login_server_create(OnLog *on_log, CreateUserContext *create_user_context, DestroyUserContext destroy_user_ctx, OnClientCreate *on_client_create, OnClientConnect *on_client_connect, OnClientPacket *on_client_packet, OnClientDisconnect *on_client_disconnect, OnClientDestroy *on_client_destroy, OnClientLeave *on_client_leave);
void login_server_destroy(struct LoginServer *server);
enum ResponderResult login_server_start(struct LoginServer *server);
void login_server_stop(struct LoginServer *server);
int assign_channel(uint32_t id, uint8_t world, uint8_t channel);

int session_get_event_disposition(struct Session *session);
int session_set_event(struct Session *session, int status, int fd, OnResume *on_resume);
void session_write(struct Session *session, size_t length, uint8_t *packet);

#endif


#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "worker.h"
//#include "thread-pool.h"
//#include "cmd.h"

struct Session;

struct Buf;

typedef void Work(struct Session *session, void *);
typedef void OnSessionCreate(struct Session *session, void *);
typedef void OnSessionRead(struct Session *session, struct Buf *, void *);
typedef void OnSessionWrite(struct Session *session, void *);

int session_create(Worker worker, int fd, OnSessionCreate *on_created, void *user_data);
void session_destroy(struct Session *session);
void session_shutdown(struct Session *session);
void session_set_id(struct Session *session, uint32_t id);
uint32_t session_id(struct Session *session);
void session_set_worker(struct Session *session, Worker worker);
int session_write(struct Session *session, size_t len, uint8_t *packet);
void session_wait_for_writes(struct Session *session, Work *work, void *user_data);
//int session_send_command(struct Session *session, struct SessionCommand *cmd);
uint32_t session_id(struct Session *session);

uint8_t *buf_frame(struct Buf *buf);
void buf_ack(struct Buf *buf);

#endif


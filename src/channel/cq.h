#ifndef CQ_H
#define CQ_H

#include <stddef.h>
#include <stdint.h>

#include "cmd.h"

struct CommandQueue;

struct SessionCommandContext {
    struct Session *session;
    struct SessionCommand cmd;
};

struct CommandQueue *cq_create(int fd);
void cq_destroy(struct CommandQueue *cq);
int cq_enqueue(struct CommandQueue *cq, struct SessionCommandContext *cmd);
void cq_peek(struct CommandQueue *cq, size_t len, struct SessionCommandContext *cmd);
void cq_dequeue(struct CommandQueue *cq, size_t count);

#endif


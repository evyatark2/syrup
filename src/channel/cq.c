#include "cq.h"

#include <sys/eventfd.h>
#include <stdlib.h>
#include <threads.h>

#include "io-worker.h"

struct CommandQueue {
    int fd;
    mtx_t mtx;
    struct SessionCommandContext *queue;
    size_t capacity;
    size_t start;
    size_t len;
};

struct CommandQueue *cq_create(int fd)
{
    struct CommandQueue *cq = malloc(sizeof(struct CommandQueue));
    if (cq == NULL)
        return NULL;

    cq->queue = malloc(sizeof(struct SessionCommandContext));
    if (cq->queue == NULL) {
        free(cq);
        return NULL;
    }

    if (mtx_init(&cq->mtx, mtx_plain) != thrd_success) {
        free(cq->queue);
        free(cq);
        return NULL;
    }

    cq->fd = fd;
    cq->capacity = 1;
    cq->start = 0;
    cq->len = 0;

    return cq;
}

void cq_destroy(struct CommandQueue *cq)
{
    if (cq != NULL) {
        mtx_destroy(&cq->mtx);
        free(cq->queue);
    }
    free(cq);
}

int cq_enqueue(struct CommandQueue *cq, struct SessionCommandContext *ctx)
{
    mtx_lock(&cq->mtx);
    if (cq->len == cq->capacity) {
        void *temp = realloc(cq->queue, (cq->capacity * 2) * sizeof(struct SessionCommandContext));
        if (temp == NULL) {
            mtx_unlock(&cq->mtx);
            return -1;
        }

        cq->queue = temp;
        cq->capacity *= 2;
    }

    cq->queue[(cq->start + cq->len) % cq->capacity] = *ctx;
    cq->len++;
    int status = eventfd_write(cq->fd, 1); // TODO: Check if it is possible to move this syscall outside the mutex lock
    if (status == -1)
        cq->len--;
    mtx_unlock(&cq->mtx);

    return status;
}

void cq_peek(struct CommandQueue *cq, size_t len, struct SessionCommandContext *cmd)
{
    mtx_lock(&cq->mtx);
    for (size_t i = 0; i < len; i++)
        cmd[i] = cq->queue[(cq->start + i) % cq->capacity];
    mtx_unlock(&cq->mtx);
}

void cq_dequeue(struct CommandQueue *cq, size_t count)
{
    mtx_lock(&cq->mtx);
    cq->start += count;
    cq->start %= cq->capacity;
    cq->len -= count;
    mtx_unlock(&cq->mtx);
}


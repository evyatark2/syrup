#define _POSIX_C_SOURCE 200809L // 200112 - posix_memalign(); 200809 - AT_FDCWD
#define _XOPEN_SOURCE 500 // for liburing's sigset_t
#include "worker.h"

#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

#include <liburing.h>

#define QUEUE_DEPTH 256
#define BUFFER_COUNT 16

struct BufGroup;

struct Worker {
    struct io_uring uring;
    struct io_uring_buf_ring *bufferRing;
    size_t pageSize;
    size_t nextCapacity;
    size_t capacity;
    size_t count;
    struct BufGroup *groups;
};

enum OpType {
    OP_READ,
    OP_WRITE,
    OP_ACCEPT
};

struct Op {
    enum OpType type;
    void *userData;
    union {
        struct {
            OnRead *onRead;
        } read;

        struct {
            OnWrite *onWrite;
            OnWriteComplete *onWriteComplete;
        } write;
    };
};

struct BufGroup {
    size_t count;
    uint8_t *base;
    struct Buf *bufs;
    uint16_t *refs;
};

struct Buf {
    struct Worker *worker;
    uint16_t bid;
    size_t size;
    uint8_t *data;
};

static size_t split(uint16_t bid, uint16_t *bgid);
static uint16_t combine(uint16_t bgid, size_t bid);

Worker worker_create()
{
    struct Worker *worker = malloc(sizeof(struct Worker));
    if (worker == NULL)
        goto fail;

    worker->bufferRing = NULL;
    worker->groups = NULL;

    if (io_uring_queue_init(QUEUE_DEPTH, &worker->uring,
            IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN) == -1)
        goto fail;

    worker->pageSize = sysconf(_SC_PAGESIZE);

    worker->groups = malloc(sizeof(struct BufGroup));
    if (worker->groups == NULL)
        goto fail;

    worker->groups[0].base = NULL;
    worker->groups[0].bufs = NULL;

    worker->groups[0].base = malloc(BUFFER_COUNT * worker->pageSize);
    if (worker->groups[0].base == NULL)
        goto fail;

    int ret;
    worker->bufferRing = io_uring_setup_buf_ring(&worker->uring, BUFFER_COUNT, 0, 0, &ret);
    if (worker->bufferRing == NULL)
        goto fail;

    worker->groups[0].bufs = malloc(BUFFER_COUNT * sizeof(struct Buf));
    if (worker->groups[0].bufs == NULL)
        goto fail;

    for (size_t i = 0; i < BUFFER_COUNT; i++) {
        worker->groups[0].bufs[i].worker = worker;
        worker->groups[0].bufs[i].bid = i;
        io_uring_buf_ring_add(worker->bufferRing, worker->groups[0].base + i * worker->pageSize, worker->pageSize, i, io_uring_buf_ring_mask(BUFFER_COUNT), i);
    }

    io_uring_buf_ring_advance(worker->bufferRing, BUFFER_COUNT);

    worker->nextCapacity = 1;
    worker->capacity = 1;
    worker->count = 1;
    worker->groups[0].count = BUFFER_COUNT;

    return worker;

fail:
    if (worker != NULL) {
        if (worker->groups != NULL) {
            free(worker->groups[0].base);
            free(worker->groups[0].bufs);
        }

        free(worker->groups);
        io_uring_free_buf_ring(&worker->uring, worker->bufferRing, BUFFER_COUNT, 0);
        io_uring_queue_exit(&worker->uring);
    }
    free(worker);
    return NULL;
}

void worker_destroy(Worker worker)
{
    if (worker != NULL) {
        io_uring_queue_exit(&worker->uring);
    }

    free(worker);
}

int worker_run(Worker worker)
{
    while (io_uring_submit_and_get_events(&worker->uring) >= 0) {
        struct io_uring_cqe *cqe;
        do {
            io_uring_wait_cqe(&worker->uring, &cqe);
            struct Op *op = (void *)cqe->user_data;
            switch (op->type) {
            case OP_READ: {
                uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                uint16_t bgid;
                bid = split(bid, &bgid);
                struct BufGroup *group = &worker->groups[bgid];
                group->count--;
                struct Buf *buf = &group->bufs[group->count];
                buf->bid = bid;
                buf->data = group->base + bid * worker->pageSize;
                buf->size = cqe->res;
                op->read.onRead(buf, op->userData);
            }
                break;

            default: {
                static_assert(0);
            }
            }
            io_uring_cqe_seen(&worker->uring, cqe);
        } while (io_uring_cq_ready(&worker->uring) > 0);

        // We defer the group deallocation to here because if we would do it inside the callback
        // then there could be a situation where the next iteration uses a buffer that was written to
        size_t capacity = (0x8 << worker->groupCapacity);
        size_t prev_capacity = worker->groupCapacity;
        while (worker->groupCount < worker->groupCapacity && worker->groups[worker->groupCapacity - 1].count == capacity) {
            free(worker->groups[worker->groupCapacity - 1].base);
            free(worker->groups[worker->groupCapacity - 1].bufs);
            worker->groupCapacity--;
            capacity >>= 1;
        }

        if (prev_capacity != worker->groupCapacity) {
            worker->groups = realloc(worker->groups, worker->groupCapacity * sizeof(struct BufGroup));

            io_uring_free_buf_ring(&worker->uring, worker->bufferRing, worker->bufferCapacity, 0);

            int ret;
            worker->bufferRing = io_uring_setup_buf_ring(&worker->uring, worker->bufferCount, 0, 0, &ret);

            io_uring_buf_ring_init(worker->bufferRing);

            for (size_t i = 0; i < worker->groupCount; i++) {
                struct BufGroup *group = &worker->groups[i];
                for (size_t j = 0; j < group->count; j++) {
                    struct Buf *buf = &group->bufs[j];
                    io_uring_buf_ring_add(worker->bufferRing, buf->data, worker->pageSize, buf->bid, io_uring_buf_ring_mask(worker->bufferCount), j);
                }

                io_uring_buf_ring_advance(worker->bufferRing, group->count);
            }
        }
    }

    return 0;
}

int worker_read(Worker worker, int fd, bool ready, OnRead *on_read, void *user_data)
{
    struct Op *ctx = malloc(sizeof(struct Op));
    if (ctx == NULL)
        return -1;

    if (worker->bufferCount == worker->bufferCapacity) {
        struct BufGroup *group;
        void *temp = realloc(worker->groups, (worker->groupCapacity + 1) * sizeof(struct BufGroup));
        if (temp == NULL) {
            free(ctx);
            return -1;
        }

        worker->groups = temp;

        group = &worker->groups[worker->groupCount];

        group->base = malloc(worker->bufferCapacity * worker->pageSize);
        if (group->base == NULL) {
            free(ctx);
            return -1;
        }

        group->bufs = malloc(worker->bufferCapacity * sizeof(struct Buf));
        if (group->bufs == NULL) {
            free(group->base);
            free(group);
            free(ctx);
            return -1;
        }

        group->count = worker->bufferCapacity;

        worker->groupCapacity++;
        worker->groupCount = worker->groupCapacity;

        io_uring_free_buf_ring(&worker->uring, worker->bufferRing, worker->bufferCapacity, 0);
        int ret;
        worker->bufferRing = io_uring_setup_buf_ring(&worker->uring, worker->bufferCapacity * 2, 0, 0, &ret);
        if (worker->bufferRing == NULL)
            ; // TODO

        for (size_t i = 0; i < worker->bufferCapacity; i++) {
            group->bufs[i].worker = worker;
            group->bufs[i].bid = worker->bufferCapacity + i;
            io_uring_buf_ring_add(worker->bufferRing, group->base + i * worker->pageSize, worker->pageSize, worker->bufferCapacity + i, io_uring_buf_ring_mask(worker->bufferCapacity * 2), i);
        }

        io_uring_buf_ring_advance(worker->bufferRing, worker->bufferCapacity);

        group->count = worker->bufferCapacity;
        worker->bufferCapacity *= 2;
        worker->groupCount++;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&worker->uring);
    if (sqe == NULL) {
        io_uring_submit(&worker->uring);
        sqe = io_uring_get_sqe(&worker->uring);
    }
    io_uring_prep_recv(sqe, fd, NULL, worker->pageSize, 0);
    if (!ready)
        sqe->ioprio = IORING_RECVSEND_POLL_FIRST;

    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = 0;

    io_uring_sqe_set_data(sqe, ctx);

    ctx->onRead = on_read;
    ctx->userData = user_data;

    return 0;
}

size_t buf_size(Buf buf)
{
    return buf->size;
}

uint8_t *buf_data(Buf buf)
{
    return buf->data;
}

void buf_ack(Buf buf)
{
    struct Worker *worker = buf->worker;
    worker->bufferCount--;
    if (worker->bufferCount < worker->bufferCapacity / 4 && worker->bufferCapacity / 2 >= BUFFER_COUNT) {
        worker->bufferCapacity /= 2;
    }

    if (buf->bid < worker->bufferCapacity) {
        io_uring_buf_ring_add(worker->bufferRing, buf->data, worker->pageSize, buf->bid, io_uring_buf_ring_mask(worker->bufferCapacity), 0);
        io_uring_buf_ring_advance(worker->bufferRing, 1);
    }

    uint16_t bgid;
    uint16_t bid = split(buf->bid, &bgid);
    struct BufGroup *group = &worker->groups[bgid];

    group->refs[bid] = group->count;
    group->count++;
}

static size_t split(uint16_t bid, uint16_t *bgid)
{
    uint16_t temp = bid;
    *bgid = 0;
    while (temp >>= 1)
        (*bgid)++;

    *bgid = *bgid < 4 ? 0 : *bgid - 3;

    return bid - ((1 << (*bgid + 3)) & 0xFFF0);
}

static uint16_t combine(uint16_t bgid, size_t bid)
{
    if (bgid == 0)
        return bid;

    return (1 << (bgid + 3)) + bid;
}


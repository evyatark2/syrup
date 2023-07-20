#define _XOPEN_SOURCE 500 // Needed for liburing.h to expose sigset_t
#define _POSIX_C_SOURCE 200809L // Needed for liburing.h to expose AT_FDCWD

#include "thread-pool.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <liburing.h>

struct Command {
    CommandWork *command;
    void *userData;
};

struct MessageQueue {
    int efd;
    mtx_t mtx;
    bool closing;
    size_t capacity;
    size_t start;
    size_t len;
    struct Command *queue;
};

static int mq_init(struct MessageQueue *);
static void mq_close(struct MessageQueue *mq);
static void mq_terminate(struct MessageQueue *mq);
static int mq_post(struct MessageQueue *mq, struct Command *cmd);
static ssize_t mq_get(struct MessageQueue *mq, size_t len, struct Command *cmds);

struct Worker {
    size_t i;
    void *userData;
    struct io_uring ring;
    size_t count; // Number of submitted SQEs
    struct MessageQueue mq;
};

struct ThreadPool {
    size_t threadCount;
    thrd_t *threads;
    struct Worker *workers;
};

static int start_worker(void *ctx);

struct ThreadPool *thread_pool_create(size_t count, void **user_data)
{
    struct ThreadPool *pool = malloc(sizeof(struct ThreadPool));
    if (pool == NULL)
        return NULL;

    pool->threads = malloc(count * sizeof(thrd_t));
    if (pool->threads == NULL) {
        free(pool);
        return NULL;
    }

    pool->workers = malloc(count * sizeof(struct Worker));
    if (pool->threads == NULL) {
        free(pool->threads);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        struct Worker *worker = &pool->workers[i];
        mq_init(&worker->mq);
        if (io_uring_queue_init(8, &worker->ring,
                    IORING_SETUP_R_DISABLED | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN) == -1) {
            for (size_t j = 0; j < i; j++) {
            }
            return NULL;
        }
        worker->userData = user_data[i];

        worker->i = i;
        worker->count = 0;
        thrd_create(pool->threads + i, start_worker, &pool->workers[i]);
    }

    pool->threadCount = count;

    return pool;
}

void thread_pool_destroy(struct ThreadPool *pool)
{
    for (size_t i = 0; i < pool->threadCount; i++) {
        struct Worker *worker = &pool->workers[i];
        mq_post(&worker->mq, &(struct Command) { .command = NULL });
    }

    for (size_t i = 0; i < pool->threadCount; i++) {
        struct Worker *worker = &pool->workers[i];
        thrd_join(pool->threads[i], NULL);
        mq_terminate(&worker->mq);
    }
}

struct Worker *thread_pool_get_worker(struct ThreadPool *pool, size_t i)
{
    return &pool->workers[i];
}

enum EventType {
    EVENT_TYPE_READ,
    EVENT_TYPE_WRITE,
    EVENT_TYPE_POLL,
    EVENT_TYPE_CANCEL,
    EVENT_TYPE_TIMEOUT,
    EVENT_TYPE_COMMAND,
};

struct Event {
    enum EventType type;
    void *userData;
    union {
        ReadWork *read;
        struct {
            WriteWork *write;
            WriteAckWork *ack;
        };
        PollWork *poll;
        CancelWork *cancel;
        CommandWork *command;
    };
};

static struct io_uring_sqe *get_sqe(struct Worker *worker)
{
    struct io_uring *ring = &worker->ring;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        assert(io_uring_submit(ring) == 8);
        sqe = io_uring_get_sqe(ring);
    }

    assert(sqe != NULL);

    worker->count++;
    return sqe;
}

void *worker_get_user_data(struct Worker *worker)
{
    return worker->userData;
}

struct Event *worker_read(struct Worker *worker, int fd, void *buf, size_t len, bool cont, ReadWork *cb, void *user_data)
{
    struct Event *ev = malloc(sizeof(struct Event));
    if (ev == NULL)
        return NULL;

    struct io_uring_sqe *sqe = get_sqe(worker);
    io_uring_prep_recv(sqe, fd, buf, len, 0);
    sqe->ioprio |= cont ? 0 : IORING_RECVSEND_POLL_FIRST;

    ev->type = EVENT_TYPE_READ;
    ev->read = cb;
    ev->userData = user_data;
    io_uring_sqe_set_data(sqe, ev);
    return ev;
}

struct Event *worker_write(struct Worker *worker, int fd, void *buf, size_t len, WriteWork *cb, WriteAckWork *cb2, void *user_data)
{
    struct Event *ev = malloc(sizeof(struct Event));
    if (ev == NULL)
        return NULL;

    struct io_uring_sqe *sqe = get_sqe(worker);
    io_uring_prep_send_zc(sqe, fd, buf, len, MSG_NOSIGNAL, 0);

    ev->type = EVENT_TYPE_WRITE;
    ev->write = cb;
    ev->ack = cb2;
    ev->userData = user_data;
    io_uring_sqe_set_data(sqe, ev);
    return ev;
}

int worker_poll(struct Worker *worker, int fd, int events, PollWork *cb, void *user_data)
{
    struct Event *ev = malloc(sizeof(struct Event));
    if (ev == NULL)
        return -1;

    struct io_uring_sqe *sqe = get_sqe(worker);
    io_uring_prep_poll_add(sqe, fd, events);

    ev->type = EVENT_TYPE_POLL;
    ev->poll = cb;
    ev->userData = user_data;
    io_uring_sqe_set_data(sqe, ev);
    return 0;
}

int worker_cancel(struct Worker *worker, struct Event *ev, CancelWork *cb, void *user_data)
{
    struct Event *new = malloc(sizeof(struct Event));
    if (new == NULL)
        return -1;

    struct io_uring_sqe *sqe = get_sqe(worker);
    io_uring_prep_cancel(sqe, ev, 0);

    new->type = EVENT_TYPE_CANCEL;
    new->cancel = cb;
    new->userData = user_data;
    io_uring_sqe_set_data(sqe, new);
    return 0;
}

int worker_command(struct Worker *worker, CommandWork *cb, void *user_data)
{
    struct Command cmd = {
        .command = cb,
        .userData = user_data
    };
    return mq_post(&worker->mq, &cmd);
}

int worker_delayed_command(struct Worker *worker, struct timeval *tm, CommandWork *cb, void *user_data)
{
    struct Event *ev = malloc(sizeof(struct Event));
    if (ev == NULL)
        return -1;

    struct io_uring_sqe *sqe = get_sqe(worker);
    struct __kernel_timespec ts = {
        .tv_sec = tm->tv_sec,
        .tv_nsec = tm->tv_usec * 1000
    };
    io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_ABS | IORING_TIMEOUT_ETIME_SUCCESS);

    ev->type = EVENT_TYPE_TIMEOUT;
    ev->command = cb;
    ev->userData = user_data;
    io_uring_sqe_set_data(sqe, ev);
    return 0;
}

static int start_worker(void *ctx)
{
    struct Worker *worker = ctx;

    char name[16];
    snprintf(name, 16, "Worker #%zu", worker->i);
    prctl(PR_SET_NAME, name);

    io_uring_enable_rings(&worker->ring);

    eventfd_t count;
    struct io_uring_sqe *sqe = get_sqe(worker);
    io_uring_prep_read(sqe, worker->mq.efd, &count, sizeof(eventfd_t), 0);
    struct Event cmd_ev = {
        .type = EVENT_TYPE_COMMAND,
    };
    io_uring_sqe_set_data(sqe, &cmd_ev);

    while (true) {
        if (worker->count == 0)
            break;

        if (io_uring_submit_and_wait(&worker->ring, 1) == -EINTR)
            continue;

        struct io_uring_cqe *cqe;
        while (io_uring_peek_cqe(&worker->ring, &cqe) == 0) {
            if (!(cqe->flags & IORING_CQE_F_MORE))
                worker->count--;

            struct Event *ev = io_uring_cqe_get_data(cqe);
            //struct Job job = mq_get(&worker->mq);

            //printf("res: %d, flags: %d, data: %llu\n", cqe->res, cqe->flags, cqe->user_data);
            //printf("%d\n", ev->type);
            switch (ev->type) {
            case EVENT_TYPE_READ:
                ev->read(worker, cqe->res, ev->userData);
            break;

            case EVENT_TYPE_WRITE:
                if (cqe->flags & IORING_CQE_F_MORE)
                    ev->write(worker, cqe->res, ev->userData);
            break;

            case EVENT_TYPE_POLL:
                ev->poll(worker, cqe->res, ev->userData);
            break;

            case EVENT_TYPE_CANCEL:
                ev->cancel(worker, cqe->res, ev->userData);
            break;

            case EVENT_TYPE_COMMAND: {
                struct Command cmds[count];
                count = mq_get(&worker->mq, count, cmds);
                for (size_t i = 0; i < count; i++) {
                    if (cmds[i].command == NULL) {
                        assert(i == count - 1); // The close command should be the last command posted
                        mq_close(&worker->mq);
                        break;
                    }
                    cmds[i].command(worker, cmds[i].userData);
                }

                if (worker->mq.efd != -1) {
                    struct io_uring_sqe *sqe = get_sqe(worker);
                    io_uring_prep_read(sqe, worker->mq.efd, &count, sizeof(eventfd_t), 0);
                    io_uring_sqe_set_data(sqe, ev);
                }
            }
            break;

            default:
                assert(false);
            }

            if (ev->type != EVENT_TYPE_COMMAND && !(cqe->flags & IORING_CQE_F_MORE))
                free(ev);

            io_uring_cqe_seen(&worker->ring, cqe);
        }
    }

    io_uring_queue_exit(&worker->ring);
    return 0;
}

static int mq_init(struct MessageQueue *mq)
{
    mq->efd = eventfd(0, 0);
    if (mq->efd == -1)
        return -1;

    mq->queue = malloc(sizeof(struct Command));
    if (mq->queue == NULL) {
        close(mq->efd);
        return -1;
    }

    if (mtx_init(&mq->mtx, mtx_plain) != thrd_success) {
        free(mq->queue);
        close(mq->efd);
        return -1;
    }

    mq->closing = false;
    mq->capacity = 1;
    mq->start = 0;
    mq->len = 0;
    return 0;
}

static void mq_close(struct MessageQueue *mq)
{
    close(mq->efd);
}

static void mq_terminate(struct MessageQueue *mq)
{
    mtx_destroy(&mq->mtx);
    free(mq->queue);
}

static int mq_post(struct MessageQueue *mq, struct Command *cmd)
{
    bool close = cmd->command == NULL;

    mtx_lock(&mq->mtx);
    if (mq->closing) {
        mtx_unlock(&mq->mtx);
        return -1;
    }

    if (mq->len == mq->capacity) {
        void *temp = realloc(mq->queue, (mq->capacity * 2) * sizeof(struct Command));
        if (temp == NULL) {
            mtx_unlock(&mq->mtx);
            return -1;
        }

        mq->queue = temp;
        mq->capacity *= 2;
    }
    size_t i = (mq->start + mq->len) % mq->capacity;
    mq->queue[i] = *cmd;
    mq->len++;
    eventfd_write(mq->efd, 1);
    if (close)
        mq->closing = true;
    mtx_unlock(&mq->mtx);
    return 0;
}

static ssize_t mq_get(struct MessageQueue *mq, size_t len, struct Command *cmds)
{
    size_t i_;
    mtx_lock(&mq->mtx);
    for (i_ = 0; i_ < len; i_++) {
        if (mq->len == 0)
            break;

        size_t i = (mq->start + i_) % mq->capacity;
        cmds[i_] = mq->queue[i];
    }
    mq->len -= i_;
    mq->start += i_;
    mq->start %= mq->capacity;
    mtx_unlock(&mq->mtx);
    return i_;
}


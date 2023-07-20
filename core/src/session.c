#include "session.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "crypt.h"

struct Session {
    uint32_t id;
    worker;
    int fd;
    struct EncryptionContext *enc;
    struct DecryptionContext *dec;

    Work *work;
    void *userData;
    
    size_t capacity;
    size_t start;
    size_t len;
    uint8_t *data;
    struct Event *wr;

    struct CommandQueue *cq;
};

struct Buf {
    size_t len;
    size_t *sizes;
    size_t i;
    size_t offset;
    uint8_t *data;
};

struct CommandQueue {
    struct SessionCommandContext *commands;
    size_t capacity;
    size_t start;
    size_t len;
};

int session_create(Worker worker, int fd, OnSessionCreate *on_created, void *user_data)
{
    struct Session *session = malloc(sizeof(struct Session));
    if (session == NULL)
        return -1;

    session->data = malloc(1);
    if (session->data == NULL) {
        free(session);
        return -1;
    }

    session->enc = encryption_context_new((uint8_t[]) { 0, 0, 0, 0 } );
    if (session->enc == NULL) {
        free(session->data);
        free(session);
        return -1;
    }

    session->dec = decryption_context_new((uint8_t[]) { 0, 0, 0, 0 } );
    if (session->dec == NULL) {
        encryption_context_destroy(session->enc);
        free(session->data);
        free(session);
        return -1;
    }

    session->fd = fd;
    session->capacity = 1;
    session->start = 0;
    session->len = 0;
    session->work = NULL;

    worker_write();
    //session->command = io_worker_wait_on_continuous

    return 0;
}

void session_shutdown(struct Session *session)
{
    shutdown(session->fd, SHUT_RD);
}

void session_set_id(struct Session *session, uint32_t id)
{
    session->id = id;
}

uint32_t session_id(struct Session *session)
{
    return session->id;
}

void session_destroy(struct Session *session)
{
    encryption_context_destroy(session->enc);
}

int session_send_command(struct Session *session, struct SessionCommand *cmd)
{
    /*struct SessionCommandContext ctx;

    ctx.session = session;
    ctx.cmd = *cmd;

    worker_command(session->worker, on_session_command, ctx);
    if (cq_enqueue(session->cq, &ctx) == -1) {
        session_shutdown(session);
        return -1;
    }*/

    return 0;
}

void session_set_worker(struct Session *session, struct Worker *worker)
{
    session->worker = worker;
}

static void on_session_write(struct Worker *, ssize_t, void *);
static void on_session_write_ack(struct Worker *);
static void on_session_write_cancelled(struct Worker *, int, void *);

static bool continue_write(struct Session *session);

int session_write(struct Session *session, size_t len, uint8_t *packet)
{
    if (session->len + len + 4 > session->capacity) {
        if (session->wr != NULL)
            return worker_cancel(session->worker, session->wr, on_session_write_cancelled, session);

        size_t n = session->len + len + 4;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        n++;

        void *temp = realloc(session->data, n);
        if (temp == NULL) {
            session_shutdown(session);
            return -1;
        }

        session->data = temp;
        session->capacity = n;
    }

    if (session->start + session->len + len + 4 > session->capacity) {
        uint8_t header[4];
        encryption_context_header(session->enc, len, header);
        if (session->start + 4 > session->capacity) {
            memcpy(session->data + session->start, header, session->capacity - session->start);
            memcpy(session->data, header + (session->capacity - session->start), 4 - (session->capacity - session->start));

            session->start += 4;
            session->start %= session->capacity;

            memcpy(session->data + session->start, packet, len);
            encryption_context_encrypt(session->enc, len, session->data + session->start);
            session->start += len;
        } else {
            memcpy(session->data + session->start, header, 4);
            encryption_context_encrypt(session->enc, len, packet);
            memcpy(session->data + session->start, packet, session->capacity - session->start);

            session->start += 4;
            session->start %= session->capacity;

            memcpy(session->data + session->start, packet, session->capacity - session->start);
            memcpy(session->data, packet + (session->capacity - session->start), len - (session->capacity - session->start));
            session->start += len - (session->capacity - session->start);
        }
    } else {
        memcpy(session->data + session->start + 4, packet, len);
        encryption_context_header(session->enc, len, session->data + session->start);
        encryption_context_encrypt(session->enc, len, session->data + session->start + 4);
    }

    session->len += len + 4;

    printf("Writing:\n");
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", packet[i]);
    }
    puts("");

    if (session->wr == NULL) {
        continue_write(session);
        if (session->wr == NULL)
            return -1;
    }
    return 0;
}

static void on_session_write(struct Worker *worker, ssize_t len, void *user_data)
{
    struct Session *session = user_data;
    if (len == 0)
        return;

    if (len == -1) {
        session_shutdown(session);
        return;
    }

    session->len -= len;
    session->start += len;
    session->start %= session->capacity;
    
    if (session->len > 0) {
        if (!continue_write(session))
            session_shutdown(session);
    }
}

static void on_session_write_ack(struct Worker *worker)
{
}

static void on_session_write_cancelled(struct Worker *, int status, void *user_data)
{
}

static bool continue_write(struct Session *session)
{
    session->wr = worker_write(session->worker, session->fd, session->data,
        session->start + session->len > session->capacity ? session->capacity - session->len : session->len,
        on_session_write, on_session_write_ack, session);
    return session->wr != NULL;
}

void session_wait_for_writes(struct Session *session, Work *work, void *user_data)
{
    if (session->len == 0) {
        work(session, user_data);
        return;
    }

    session->work = work;
    session->userData = user_data;
}

uint8_t *buf_frame(struct Buf *buf)
{
    if (buf->i == buf->len)
        return NULL;

    uint8_t *frame = buf->data + buf->offset;
    buf->offset += buf->sizes[buf->i];
    buf->i++;
    return frame;
}

void buf_ack(struct Buf *buf);


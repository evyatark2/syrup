#include <stdio.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "src/worker.h"

static void on_read(Buf buf, void *user_data);

struct ctx {
    Worker worker;
    int fd;
};

int main() {
    Worker worker = worker_create();
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(0x7F000001),
        .sin_port = htons(7575)
    };
    if (connect(fd, (void *)&addr, sizeof(struct sockaddr_in)) == -1)
        return -1;

    struct ctx ctx = {
        .worker = worker,
        .fd = fd
    };
    worker_read(worker, fd, false, on_read, &ctx);

    worker_run(worker);
}

static void on_read(Buf buf, void *user_data)
{
    struct ctx *ctx = user_data;
    printf("Got packet with length %zu\n", buf_size(buf));
    buf_ack(buf);
    worker_read(ctx->worker, ctx->fd, false, on_read, ctx);
}


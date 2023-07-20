#ifndef WORKER_H
#define WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Worker *Worker;
typedef struct Buf *Buf;

typedef void OnRead(Buf, void *user_data);
typedef void OnWrite(Buf, void *user_data);
typedef void OnWriteComplete(Buf, void *user_data);

Worker worker_create();
void worker_destroy(Worker worker);
int worker_run(Worker worker);
int worker_read(Worker worker, int fd, bool ready, OnRead *on_read, void *user_data);

size_t buf_size(Buf buf);
uint8_t *buf_data(Buf buf);
void buf_ack(Buf buf);

#endif


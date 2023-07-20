#include <stddef.h>

#include <sys/socket.h>
#include <sys/time.h>

struct Worker;

typedef void ReadWork(struct Worker *, ssize_t, void *);
typedef void WriteWork(struct Worker *, ssize_t, void *);
typedef void WriteAckWork(struct Worker *, void *);
typedef void PollWork(struct Worker *, int, void *);
typedef void CancelWork(struct Worker *, int, void *);
typedef void CommandWork(struct Worker *, void *);

void *worker_get_user_data(struct Worker *);

struct Event;

/**
 * Submit an async read request into buf with length len.
 * 
 * \p worker The worker to submit the request on
 * \p fd The file descriptor to read on
 * \p buf The buffer
 * \p len The buffer's length
 * \p cont Whether this read request is a continuation of a previous read request.
 *  set this to true when you recieved a partial packet (i.e. only 14/23 bytes of the packet were recieved)
 *  set this to false when the last packet has been read in its entirety
 * \p cb The callback to run when data have been recieved
 * \p user_data Arbitrary data to be passed to the callback
 */
struct Event *worker_read(struct Worker *worker, int fd, void *buf, size_t len, bool cont, ReadWork *cb, void *user_data);
struct Event *worker_write(struct Worker *worker, int fd, void *buf, size_t len, WriteWork *cb, WriteAckWork *cb2, void *user_data);
int worker_poll(struct Worker *worker, int fd, int events, PollWork *cb, void *user_data);
int worker_cancel(struct Worker *worker, struct Event *ev, CancelWork *cb, void *user_data);
int worker_command(struct Worker *worker, CommandWork *cb, void *user_data);
int worker_delayed_command(struct Worker *worker, struct timeval *tm, CommandWork *cb, void *user_data);

struct ThreadPool;

/**
 * Create a thread pool with \p count threads.
 *
 * \p count The number of threads in the pool
 * \p user_data[.count] An array that holds arbitrary user data for each thread. Use \p worker_get_user_data to get it.
 */
struct ThreadPool *thread_pool_create(size_t count, void **user_data);
void thread_pool_destroy(struct ThreadPool *pool);
struct Worker *thread_pool_get_worker(struct ThreadPool *pool, size_t i);


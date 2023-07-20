#ifndef THREAD_COORDINATOR_H
#define THREAD_COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/types.h> // ssize_t

struct RoomThreadCoordinator;

struct RoomThreadCoordinator *room_thread_coordinator_create(void);
void room_thread_coordinator_destroy(struct RoomThreadCoordinator *mgr);
ssize_t room_thread_coordinator_get(struct RoomThreadCoordinator *mgr, uint32_t map);
ssize_t room_thread_coordinator_get_init(struct RoomThreadCoordinator *mgr);
ssize_t room_thread_coordinator_ref(struct RoomThreadCoordinator *mgr, uint32_t map);
void room_thread_coordinator_unref(struct RoomThreadCoordinator *mgr, uint32_t map);

#endif


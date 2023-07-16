#ifndef THREAD_COORDINATOR_H
#define THREAD_COORDINATOR_H

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h> // ssize_t

struct MapThreadCoordinator;

struct MapThreadCoordinator *map_thread_coordinator_create(void);
void map_thread_coordinator_destroy(struct MapThreadCoordinator *mgr);
ssize_t map_thread_coordinator_get(struct MapThreadCoordinator *mgr, uint32_t map);
ssize_t map_thread_coordinator_ref(struct MapThreadCoordinator *mgr, uint32_t map);
void map_thread_coordinator_unref(struct MapThreadCoordinator *mgr, uint32_t map);

#endif


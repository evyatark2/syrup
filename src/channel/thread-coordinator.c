#include "thread-coordinator.h"

#include <stdlib.h>
#include <threads.h>

#include "../hash-map.h"

struct MapThreadCoordinator {
    mtx_t lock;
    struct HashSetU32 *mapDict;
};

struct Pair {
    uint32_t map;
    size_t thread;
};

struct MapThreadCoordinator *map_thread_coordinator_create(void)
{
    struct MapThreadCoordinator *mgr = malloc(sizeof(struct MapThreadCoordinator));
    if (mgr == NULL)
        return NULL;

    mgr->mapDict = hash_set_u32_create(sizeof(struct Pair), offsetof(struct Pair, map));
    if (mgr->mapDict == NULL) {
        free(mgr);
        return NULL;
    }

    if (mtx_init(&mgr->lock, mtx_plain) != thrd_success) {
        hash_set_u32_destroy(mgr->mapDict);
        free(mgr);
        return NULL;
    }

    return mgr;
}

void map_thread_coordinator_destroy(struct MapThreadCoordinator *mgr)
{
    if (mgr != NULL) {
        mtx_destroy(&mgr->lock);
        hash_set_u32_destroy(mgr->mapDict);
    }
}

ssize_t map_thread_coordinator_ref(struct MapThreadCoordinator *mgr, uint32_t map)
{
    struct Pair *pair;
    mtx_lock(&mgr->lock);
    pair = hash_set_u32_get(mgr->mapDict, map);
    if (pair == NULL) {
        size_t thread = 0;
        struct Pair data = {
            .map = map,
            .thread = thread,
        };
        if (hash_set_u32_insert(mgr->mapDict, &data) == -1) {
            mtx_unlock(&mgr->lock);
            return -1;
        }

        pair = hash_set_u32_get(mgr->mapDict, map);
    }

    mtx_unlock(&mgr->lock);
    return pair->thread;
}

ssize_t map_thread_coordinator_get(struct MapThreadCoordinator *mgr, uint32_t map)
{
    struct Pair *pair;
    ssize_t thread = -1;
    struct HashSetU32 *dict = mgr->mapDict;
    mtx_lock(&mgr->lock);
    pair = hash_set_u32_get(dict, map);
    if (pair != NULL)
        thread = pair->thread;
    mtx_unlock(&mgr->lock);
    return thread;
}

void map_thread_coordinator_unref(struct MapThreadCoordinator *mgr, uint32_t map)
{
    mtx_lock(&mgr->lock);
    hash_set_u32_remove(mgr->mapDict, map);
    mtx_unlock(&mgr->lock);
}


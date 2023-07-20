#include "events.h"

#include <stdlib.h>
#include <threads.h>

#include "../hash-map.h"

struct EventContext {
    Wait *wait;
    void *userData;
    struct Event *ev;
    void (*f)(struct EventContext *);
};

#ifdef X
#  undef X
#endif

#define X(transport) \
    static void transport##_arrive(struct EventContext *); \
    static void transport##_close_gates(struct EventContext *); \
    static void transport##_depart(struct EventContext *);
TRANSPORT_EVENTS()
#undef X

static void set_and_sched(struct EventContext *e, uint32_t prop, int32_t value, uint32_t sec, void (*f)(struct EventContext *));

#define DEFINE_TRANSPORT_EVENT_FUNCTIONS(event, eid, prop, asec, csec, dsec) \
    int event_##event##_init(struct EventManager *mgr, Wait *wait, void *user_data) \
    { \
        struct EventContext *ctx = malloc(sizeof(struct EventContext)); \
        if (ctx == NULL) \
            return -1; \
        ctx->wait = wait; \
        ctx->userData = user_data; \
        ctx->ev = event_manager_get_event(mgr, eid); \
        event##_arrive(ctx); \
        return 0; \
    } \
 \
    static void event##_arrive(struct EventContext *e) \
    { \
        set_and_sched(e, prop, 0, csec, event##_close_gates); \
    } \
 \
    static void event##_close_gates(struct EventContext *e) \
    { \
        set_and_sched(e, prop, 1, dsec, event##_depart); \
    } \
 \
    static void event##_depart(struct EventContext *e) \
    { \
        set_and_sched(e, prop, 2, asec, event##_arrive); \
    }

DEFINE_TRANSPORT_EVENT_FUNCTIONS(boat, EVENT_BOAT, EVENT_BOAT_PROPERTY_SAILING, 10*60, 4*60, 1*60)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(train, EVENT_TRAIN, EVENT_TRAIN_PROPERTY_SAILING, 5*60, 4*60, 1*60)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(subway, EVENT_SUBWAY, EVENT_SUBWAY_PROPERTY_SAILING, 4*60, 50, 10)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(genie, EVENT_GENIE, EVENT_GENIE_PROPERTY_SAILING, 5*60, 4*60, 1*60)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(airplane, EVENT_AIRPLANE, EVENT_AIRPLANE_PROPERTY_SAILING, 1*60, 4*60, 1*60)

static void on_event_ready(void *);
static void set_and_sched(struct EventContext *e, uint32_t prop, int32_t value, uint32_t sec, void (*f)(struct EventContext *))
{
    event_set_property(e->ev, prop, value);
    e->f = f;

    e->wait(sec, on_event_ready, e, e->userData);
}

static void on_event_ready(void *ctx)
{
    struct EventContext *e = ctx;
    e->f(e);
}

static void elevator_top(struct EventContext *e);
static void elevator_descend(struct EventContext *e);
static void elevator_bottom(struct EventContext *e);
static void elevator_ascend(struct EventContext *e);

int event_elevator_init(struct EventManager *mgr, Wait *wait, void *user_data)
{
    struct EventContext *e = malloc(sizeof(struct EventContext));
    if (e == NULL)
        return -1;
    e->wait = wait;
    e->ev = event_manager_get_event(mgr, EVENT_ELEVATOR);
    e->userData = user_data;
    elevator_top(e);
    return 0;
}

static void elevator_top(struct EventContext *e)
{
    set_and_sched(e, EVENT_ELEVATOR_PROPERTY_SAILING, 0, 60, elevator_descend);
}

static void elevator_descend(struct EventContext *e)
{
    set_and_sched(e, EVENT_ELEVATOR_PROPERTY_SAILING, 1, 60, elevator_bottom);
}

static void elevator_bottom(struct EventContext *e)
{
    set_and_sched(e, EVENT_ELEVATOR_PROPERTY_SAILING, 2, 60, elevator_ascend);
}

static void elevator_ascend(struct EventContext *e)
{
    set_and_sched(e, EVENT_ELEVATOR_PROPERTY_SAILING, 3, 60, elevator_top);
}

static void area_boss_reset(void *ctx);

static mtx_t MAPS_MTX;
static struct HashSetU32 *MAPS;

int event_area_boss_init(struct EventManager *mgr, Wait *wait, void *user_data)
{
    MAPS = hash_set_u32_create(sizeof(uint32_t), 0);
    if (MAPS == NULL) {
        return -1;
    }

    if (mtx_init(&MAPS_MTX, mtx_plain) != thrd_success) {
        hash_set_u32_destroy(MAPS);
        return -1;
    }

    struct EventContext *e = malloc(sizeof(struct EventContext));
    if (e == NULL) {
        mtx_destroy(&MAPS_MTX);
        hash_set_u32_destroy(MAPS);
        return -1;
    }

    e->wait = wait;
    e->userData = user_data;
    e->ev = event_manager_get_event(mgr, EVENT_AREA_BOSS);

    area_boss_reset(e);
    return 0;
}

static void area_boss_reset(void *ctx)
{
    struct EventContext *e = ctx;
    mtx_lock(&MAPS_MTX);
    hash_set_u32_clear(MAPS);
    mtx_unlock(&MAPS_MTX);
    event_set_property(e->ev, EVENT_AREA_BOSS_PROPERTY_RESET, 0);
    e->wait(15, area_boss_reset, e, e->userData);
}

bool event_area_boss_register(uint32_t map)
{
    mtx_lock(&MAPS_MTX);
    bool spawned = hash_set_u32_get(MAPS, map) != NULL;
    if (!spawned)
        hash_set_u32_insert(MAPS, &map);

    mtx_unlock(&MAPS_MTX);
    return spawned;
}

static void event_global_respawn_reset(void *ctx);

int event_global_respawn_init(struct EventManager *mgr, Wait *wait, void *user_data)
{
    struct EventContext *e = malloc(sizeof(struct EventContext));
    if (e == NULL)
        return -1;

    e->wait = wait;
    e->userData = user_data;
    e->ev = event_manager_get_event(mgr, EVENT_GLOBAL_RESPAWN);

    event_global_respawn_reset(e);
    return 0;
}

static void event_global_respawn_reset(void *ctx_)
{
    struct EventContext *e = ctx_;
    event_set_property(e->ev, 0, 0);
    e->wait(10, event_global_respawn_reset, e, e->userData);
}


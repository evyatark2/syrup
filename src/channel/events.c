#include "events.h"

#include <threads.h>

#include "../hash-map.h"

#define DECLARE_TRANSPORT_EVENT_STATIC_FUNCTIONS(event) \
    static void event##_arrive(struct Event *, void *); \
    static void event##_close_gates(struct Event *, void *); \
    static void event##_depart(struct Event *, void *);

DECLARE_TRANSPORT_EVENT_STATIC_FUNCTIONS(boat)
DECLARE_TRANSPORT_EVENT_STATIC_FUNCTIONS(train)
DECLARE_TRANSPORT_EVENT_STATIC_FUNCTIONS(subway)
DECLARE_TRANSPORT_EVENT_STATIC_FUNCTIONS(genie)
DECLARE_TRANSPORT_EVENT_STATIC_FUNCTIONS(airplane)
DECLARE_TRANSPORT_EVENT_STATIC_FUNCTIONS(elevator)

static void set_and_sched(struct Event *e, uint32_t prop, int32_t value, uint32_t sec, void (*f)(struct Event *, void *));

#define DEFINE_TRANSPORT_EVENT_FUNCTIONS(event, eid, prop, asec, csec, dsec) \
    void event_##event##_init(struct ChannelServer *server) \
    { \
        event##_arrive(channel_server_get_event(server, eid), NULL); \
    } \
 \
    static void event##_arrive(struct Event *e, void *ctx_) \
    { \
        set_and_sched(e, prop, 0, csec, event##_close_gates); \
    } \
 \
    static void event##_close_gates(struct Event *e, void *ctx_) \
    { \
        set_and_sched(e, prop, 1, dsec, event##_depart); \
    } \
 \
    static void event##_depart(struct Event *e, void *ctx_) \
    { \
        set_and_sched(e, prop, 2, asec, event##_arrive); \
    }

DEFINE_TRANSPORT_EVENT_FUNCTIONS(boat, EVENT_BOAT, EVENT_BOAT_PROPERTY_SAILING, 10, 4, 1)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(train, EVENT_TRAIN, EVENT_TRAIN_PROPERTY_SAILING, 10, 4, 1)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(subway, EVENT_SUBWAY, EVENT_SUBWAY_PROPERTY_SAILING, 10, 4, 1)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(genie, EVENT_GENIE, EVENT_GENIE_PROPERTY_SAILING, 10, 4, 1)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(airplane, EVENT_AIRPLANE, EVENT_AIRPLANE_PROPERTY_SAILING, 10, 4, 1)
DEFINE_TRANSPORT_EVENT_FUNCTIONS(elevator, EVENT_ELEVATOR, EVENT_ELEVATOR_PROPERTY_SAILING, 10, 4, 1)

static void set_and_sched(struct Event *e, uint32_t prop, int32_t value, uint32_t sec, void (*f)(struct Event *, void *))
{
    event_set_property(e, prop, value);
    struct timespec tm = { .tv_sec = sec, .tv_nsec = 0 };
    event_schedule(e, f, NULL, &tm);
}

static void area_boss_reset(struct Event *e, void *ctx_);

static mtx_t MAPS_MTX;
static struct HashSetU32 *MAPS;

void event_area_boss_init(struct ChannelServer *server)
{
    MAPS = hash_set_u32_create(sizeof(uint32_t), 0);
    mtx_init(&MAPS_MTX, mtx_plain);
    area_boss_reset(channel_server_get_event(server, EVENT_AREA_BOSS), NULL);
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

static void event_global_respawn_reset(struct Event *e, void *ctx);

void event_global_respawn_init(struct ChannelServer *server)
{
    event_global_respawn_reset(channel_server_get_event(server, EVENT_GLOBAL_RESPAWN), NULL);
}

static void area_boss_reset(struct Event *e, void *ctx_)
{
    mtx_lock(&MAPS_MTX);
    hash_set_u32_clear(MAPS);
    mtx_unlock(&MAPS_MTX);
    event_set_property(e, EVENT_AREA_BOSS_PROPERTY_RESET, 0);
    struct timespec tm = { .tv_sec = 15, .tv_nsec = 0 };
    event_schedule(e, area_boss_reset, NULL, &tm);
}

static void event_global_respawn_reset(struct Event *e, void *ctx)
{
    event_set_property(e, 0, 0);
    struct timespec tm = { .tv_sec = 10, .tv_nsec = 0 };
    event_schedule(e, event_global_respawn_reset, NULL, &tm);
}


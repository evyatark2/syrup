#include "events.h"

static void arrive(struct Event *e, void *ctx);
static void close_gates(struct Event *e, void *ctx);
static void depart(struct Event *e, void *ctx);

void event_ellinia_orbis_boat_init(struct ChannelServer *server)
{
    struct Event *e = channel_server_get_event(server, EVENT_ELLINIA_ORBIS_BOAT);
    arrive(e, NULL);
}

static void arrive(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ELLINIA_ORBIS_BOAT_PROPERTY_SAILING, 0);
    struct timespec tm = {
        .tv_sec = 4,
        .tv_nsec = 0
    };
    event_schedule(e, close_gates, NULL, &tm);
}

static void close_gates(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ELLINIA_ORBIS_BOAT_PROPERTY_SAILING, 1);
    struct timespec tm = {
        .tv_sec = 1,
        .tv_nsec = 0
    };
    event_schedule(e, depart, NULL, &tm);
}

static void depart(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ELLINIA_ORBIS_BOAT_PROPERTY_SAILING, 2);
    struct timespec tm = {
        .tv_sec = 15,
        .tv_nsec = 0
    };
    event_schedule(e, arrive, NULL, &tm);
}


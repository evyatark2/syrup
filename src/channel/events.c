#include "events.h"

static void boat_arrive(struct Event *e, void *ctx);
static void boat_close_gates(struct Event *e, void *ctx);
static void boat_depart(struct Event *e, void *ctx);

static void train_arrive(struct Event *e, void *ctx);
static void train_close_gates(struct Event *e, void *ctx);
static void train_depart(struct Event *e, void *ctx);

void event_ellinia_orbis_boat_init(struct ChannelServer *server)
{
    struct Event *e = channel_server_get_event(server, EVENT_ELLINIA_ORBIS_BOAT);
    boat_arrive(e, NULL);
}

void event_orbis_ludibrium_train_init(struct ChannelServer *server)
{
    struct Event *e = channel_server_get_event(server, EVENT_ORBIS_LUDIBRIUM_BOAT);
    train_arrive(e, NULL);
}

static void boat_arrive(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ELLINIA_ORBIS_BOAT_PROPERTY_SAILING, 0);
    struct timespec tm = {
        .tv_sec = 10,
        .tv_nsec = 0
    };
    event_schedule(e, boat_close_gates, NULL, &tm);
}

static void boat_close_gates(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ELLINIA_ORBIS_BOAT_PROPERTY_SAILING, 1);
    struct timespec tm = {
        .tv_sec = 5,
        .tv_nsec = 0
    };
    event_schedule(e, boat_depart, NULL, &tm);
}

static void boat_depart(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ELLINIA_ORBIS_BOAT_PROPERTY_SAILING, 2);
    struct timespec tm = {
        .tv_sec = 15,
        .tv_nsec = 0
    };
    event_schedule(e, boat_arrive, NULL, &tm);
}

static void train_arrive(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ORBIS_LUDIBRIUM_BOAT_PROPERTY_SAILING, 0);
    struct timespec tm = {
        .tv_sec = 10,
        .tv_nsec = 0
    };
    event_schedule(e, train_close_gates, NULL, &tm);

}

static void train_close_gates(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ORBIS_LUDIBRIUM_BOAT_PROPERTY_SAILING, 1);
    struct timespec tm = {
        .tv_sec = 5,
        .tv_nsec = 0
    };
    event_schedule(e, train_depart, NULL, &tm);
}

static void train_depart(struct Event *e, void *ctx)
{
    event_set_property(e, EVENT_ORBIS_LUDIBRIUM_BOAT_PROPERTY_SAILING, 2);
    struct timespec tm = {
        .tv_sec = 15,
        .tv_nsec = 0
    };
    event_schedule(e, train_arrive, NULL, &tm);
}


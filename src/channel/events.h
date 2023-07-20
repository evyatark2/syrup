#ifndef EVENTS_H
#define EVENTS_H

#include <stddef.h>
#include <stdint.h>

#include "event-manager.h"

typedef void Wait(int, void (*)(void *), void *, void *);

#define EVENT_GLOBAL_RESPAWN 0

#define EVENT_BOAT 1
#define EVENT_BOAT_PROPERTY_SAILING 0

#define EVENT_TRAIN 2
#define EVENT_TRAIN_PROPERTY_SAILING 0

#define EVENT_SUBWAY 3
#define EVENT_SUBWAY_PROPERTY_SAILING 0

#define EVENT_GENIE 4
#define EVENT_GENIE_PROPERTY_SAILING 0

#define EVENT_AIRPLANE 5
#define EVENT_AIRPLANE_PROPERTY_SAILING 0

#define EVENT_ELEVATOR 6
#define EVENT_ELEVATOR_PROPERTY_SAILING 0

#define EVENT_AREA_BOSS 7
#define EVENT_AREA_BOSS_PROPERTY_RESET 0

#define EVENT_COUNT 8

#define TRANSPORT_EVENTS() \
    X(boat) \
    X(train) \
    X(subway) \
    X(genie) \
    X(airplane) \

#ifdef X
#  undef X
#endif

#define X(transport) int event_##transport##_init(struct EventManager *, Wait *, void *);
TRANSPORT_EVENTS()
#undef X

int event_elevator_init(struct EventManager *mgr, Wait *wait, void *user_data);

int event_area_boss_init(struct EventManager *mgr, Wait *, void *);
bool event_area_boss_register(uint32_t map);

int event_global_respawn_init(struct EventManager *mgr, Wait *, void *);

#endif


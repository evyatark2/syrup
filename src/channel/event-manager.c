#include "event-manager.h"

#include <threads.h>
#include <stdlib.h>

#include "../hash-map.h"

struct EventManager {
    size_t eventCount;
    struct Event *events;
};

struct Event {
    struct HashSetU32 *properties;
    void (*f)(struct Event *, void *);
    void *ctx;
};

struct Property {
    uint32_t id;
    int32_t value;
    mtx_t mtx;
    size_t listenerCount;
    struct Listener **listeners;
};

struct Listener {
    size_t i;
    void (*f)(void *ctx);
    void *ctx;
};

struct EventManager *event_manager_create(size_t count)
{
    struct EventManager *mgr = malloc(sizeof(struct EventManager));
    if (mgr == NULL)
        return NULL;

    mgr->events = malloc(count * sizeof(struct Event));
    if (mgr->events == NULL) {
        free(mgr);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        mgr->events[i].properties = hash_set_u32_create(sizeof(struct Property), offsetof(struct Property, id));
        if (mgr->events[i].properties == NULL) {
            for (size_t j = 0; j < i; j++)
                hash_set_u32_destroy(mgr->events[j].properties);
            free(mgr->events);
            free(mgr);
            return NULL;
        }
    }

    mgr->eventCount = count;

    return mgr;
}

void event_manager_destroy(struct EventManager *mgr)
{
    free(mgr);
}

struct Event *event_manager_get_event(struct EventManager *mgr, size_t ev)
{
    if (ev >= mgr->eventCount)
        return NULL;

    return &mgr->events[ev];
}

void event_set_property(struct Event *event, uint32_t property, int32_t value)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    if (prop == NULL) {
        struct Property prop = {
            .id = property,
            .value = value,
            .listenerCount = 0,
            .listeners = NULL
        };
        mtx_init(&prop.mtx, mtx_plain);
        hash_set_u32_insert(event->properties, &prop);
    } else {
        mtx_lock(&prop->mtx);
        prop->value = value;
        for (size_t i = 0; i < prop->listenerCount; i++)
            prop->listeners[i]->f(prop->listeners[i]->ctx);
        mtx_unlock(&prop->mtx);
    }
}

bool event_has_property(struct Event *event, uint32_t property)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    return prop != NULL;
}

int32_t event_get_property(struct Event *event, uint32_t property)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    return prop->value;
}

struct Listener *event_listen(struct Event *event, uint32_t property, void (*work)(void *ctx), void *ctx)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    if (prop == NULL)
        return NULL;

    struct Listener *listener = malloc(sizeof(struct Listener));
    if (listener == NULL)
        return NULL;

    listener->f = work;
    listener->ctx = ctx;

    mtx_lock(&prop->mtx);
    void *temp = realloc(prop->listeners, (prop->listenerCount + 1) * sizeof(struct Listener *));
    if (temp == NULL) {
        mtx_unlock(&prop->mtx);
        free(listener);
        return NULL;
    }

    prop->listeners = temp;
    prop->listeners[prop->listenerCount] = listener;
    listener->i = prop->listenerCount;
    prop->listenerCount++;
    mtx_unlock(&prop->mtx);

    return listener;
}

void event_unlisten(struct Event *event, uint32_t property, struct Listener *listener)
{
    struct Property *prop = hash_set_u32_get(event->properties, property);
    if (prop == NULL)
        return;

    mtx_lock(&prop->mtx);
    prop->listeners[listener->i] = prop->listeners[prop->listenerCount - 1];
    prop->listeners[listener->i]->i = listener->i;
    prop->listenerCount--;
    mtx_unlock(&prop->mtx);

    free(listener);
}


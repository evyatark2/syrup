#ifndef EVENT_MANAGER_H
#define EVENT_MANAGER_H

#include <stddef.h>
#include <stdint.h>

struct EventManager;
struct Event;
struct Listener;

struct EventManager *event_manager_create(size_t count);
void event_manager_destroy(struct EventManager *mgr);
struct Event *event_manager_get_event(struct EventManager *mgr, size_t ev);

void event_set_property(struct Event *event, uint32_t property, int32_t value);
int32_t event_get_property(struct Event *event, uint32_t property);
struct Listener *event_listen(struct Event *event, uint32_t property, void (*work)(void *ctx), void *ctx);
void event_unlisten(struct Event *event, uint32_t property, struct Listener *listener);

#endif


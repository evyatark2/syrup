#include "server.h"

#define EVENT_BOAT 0
#define EVENT_BOAT_PROPERTY_SAILING 0

#define EVENT_TRAIN 1
#define EVENT_TRAIN_PROPERTY_SAILING 0

#define EVENT_SUBWAY 2
#define EVENT_SUBWAY_PROPERTY_SAILING 0

#define EVENT_GENIE 3
#define EVENT_GENIE_PROPERTY_SAILING 0

#define EVENT_AIRPLANE 4
#define EVENT_AIRPLANE_PROPERTY_SAILING 0

#define EVENT_ELEVATOR 5
#define EVENT_ELEVATOR_PROPERTY_SAILING 0

#define EVENT_AREA_BOSS 6
#define EVENT_AREA_BOSS_PROPERTY_RESET 0

void event_boat_init(struct ChannelServer *server);
void event_train_init(struct ChannelServer *server);
void event_subway_init(struct ChannelServer *server);
void event_genie_init(struct ChannelServer *server);
void event_airplane_init(struct ChannelServer *server);
void event_elevator_init(struct ChannelServer *server);

void event_area_boss_init(struct ChannelServer *server);
bool event_area_boss_register(uint32_t map);

#ifndef MAP_H
#define MAP_H

#include <stdint.h>

#include "server.h" // This is temporary for map_join, I should find a better way to do this
#include "../item.h"
#include "../wz.h"

struct Npc {
    uint32_t oid;
    uint32_t id;
    int16_t x;
    int16_t y;
    int16_t fh;
    int16_t cy;
    int16_t rx0;
    int16_t rx1;
    bool f;
};

struct MapHandle;
struct MapHandleContainer {
    struct MapHandle *handle;
};

enum DropType {
    DROP_TYPE_MESO,
    DROP_TYPE_ITEM,
    DROP_TYPE_EQUIP
};

struct Drop {
    uint32_t oid;
    enum DropType type;
    struct Point pos;
    union {
        struct {
            int32_t amount;
        } meso;
        struct {
            struct InventoryItem item;
        } item;
        struct {
            struct Equipment equip;
        } equip;
    };
};

struct DropBatch;

struct Map;

struct Map *map_create(struct Room *room);
void map_destroy(struct Map *map);
int map_join(struct Map *map, struct Session *session, struct MapHandleContainer *handle);
void map_leave(struct Map *map, struct MapHandle *handle);
void map_for_each_npc(struct Map *map, void (*f)(struct Npc *, void *), void *ctx);
//void map_for_each_monster(struct Map *map, void (*f)(struct Monster *, void *), void *ctx);
void map_for_each_drop(struct Map *map, void (*f)(struct Drop *, void *), void *ctx);
uint32_t map_damage_monster_by(struct Map *map, struct MapHandle *controller, uint32_t char_id, uint32_t oid, size_t hit_count, int32_t *damage);
struct MapHandle *map_switch_control(struct Map *map, struct MapHandle *new, uint32_t oid);
const struct Monster *map_get_monster(struct Map *map, uint32_t oid);
const struct Npc *map_get_npc(struct Map *map, uint32_t oid);
bool map_move_monster(struct Map *map, struct MapHandle *controller, uint8_t activity, uint32_t oid, int16_t x, int16_t y, uint16_t fh, uint8_t stance, size_t len, uint8_t *raw_data);
int32_t map_get_monster_hp(struct Map *map, uint32_t oid);
bool map_is_monster_alive(struct Map *map, uint32_t oid);
void map_make_monster_ethereal(struct Map *map, uint32_t oid);
void map_kill_monster(struct Map *map, uint32_t oid);
void map_respawn(struct Map *map);
bool map_is_monster_controlled_by(struct Map *map, struct MapHandle *controller, uint32_t oid);
void map_add_drop_batch(struct Map *map, uint32_t char_id, uint32_t monster_oid, size_t count, struct Drop *drops);
bool map_remove_drop(struct Map *map, uint32_t char_id, uint32_t oid, struct Drop *drop);
void drop_batch_for_each(struct DropBatch *batch, void (*f)(struct Drop *drop, void *ctx), void *ctx);
bool map_calculate_drop_position(struct Map *map, struct Point *p);

void monster_controller_for_each(struct MapHandle *controller, void (*f)(uint32_t oid, struct Monster *npc, struct Session *session));
struct Session *monster_controller_get_session(struct MapHandle *controller);

#endif


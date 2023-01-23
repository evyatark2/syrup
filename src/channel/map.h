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
        int32_t meso;
        struct InventoryItem item;
        struct Equipment equip;
    };
};

struct DropBatch;

struct Map;

/**
 * Creates a new map.
 *
 * \param room The room that this map is associated with
 *
 * \return The newly created map or NULL if an error occurred
 */
struct Map *map_create(struct Room *room);

/**
 * Destroys a map
 *
 * \param map The map to destroy
 */
void map_destroy(struct Map *map);

/**
 * Insert a new player to a map
 *
 * \param map The map to insert into
 * \param session The player to be inserted
 * \param[out] handle A handle that respresents the player in the map
 *
 * \return 0 if successful; -1 if an error occurred
 */
int map_join(struct Map *map, struct Session *session, struct MapHandleContainer *handle);

/**
 * Remove a player from a map
 *
 * \param map The map to remove from
 * \param handle The handle that was returned in \p map_join of the player to remove
 */
void map_leave(struct Map *map, struct MapHandle *handle);

/**
 * Do an action for each NPC in the map
 *
 * \param map The map
 * \param f The action to take
 * \param ctx Additional context to pass as the second argument to \p f
 */
void map_for_each_npc(struct Map *map, void (*f)(struct Npc *, void *), void *ctx);

/**
 * Do an action for each drop in the map
 *
 * \param map The map
 * \param f The action to take
 * \param ctx Additional context to pass as the second argument to \p f
 */
void map_for_each_drop(struct Map *map, void (*f)(struct Drop *, void *), void *ctx);

/**
 * Get if the monster is alive
 *
 * \param map The map to check in
 * \param id The monster ID - used as an additional check if the oid still refers to the same object client-side
 * \param oid The monster's object ID
 *
 * \return true if the monster is still alive
 */
bool map_monster_is_alive(struct Map *map, uint32_t id, uint32_t oid);

/**
 * Damage a monster.
 * If its health drops below 0 it will be killed and it will drop its loot.
 *
 * \param map The map the monster is in
 * \param controller The map player that damaged the monster
 * \param char_id Additional context to pass as the second argument to \p f
 * \param oid The object ID of the damaged monster
 * \param hit_count The size of \p damage array
 * \param damage An array with damage values to apply to the monster
 *
 * \return If the monster was killed, its ID; otherwise -1.
 */
uint32_t map_damage_monster_by(struct Map *map, struct MapHandle *controller, uint32_t char_id, uint32_t oid, size_t hit_count, int32_t *damage);

/**
 * Gets an NPC in the map
 *
 * \param map The map to get the NPC from.
 * \param oid The object ID of the NPC to get
 *
 * \return The requested NPC or NULL if it wasn't found.
 */
const struct Npc *map_get_npc(struct Map *map, uint32_t oid);

/**
 * Move a monster.
 *
 * Nothing will happen and false will be returned in case of any of the following:
 * * \p oid does not exists
 * * \p oid does not refer to a monster
 * * The monster is in ethereal state
 * * The map player isn't the monster's controller
 *
 * \param map The map to move the monster int.
 * \param controller The player that requested the movement
 * \param activity Raw activity - not sure what it is for now
 * \param oid The object ID of the monster to move
 * \param x The X coordinate of the new monster position
 * \param y The Y coordinate of the new monster position
 * \param fh The foothold ID of the new monster position
 * \param stance The new stance of the monster
 * \param len size of \p raw_data
 * \param raw_data Raw movement data to send to other map players
 *
 * \return true if the movement was successful; false if an error occurred.
 */
bool map_move_monster(struct Map *map, struct MapHandle *controller, uint8_t activity, uint32_t oid, int16_t x, int16_t y, uint16_t fh, uint8_t stance, size_t len, uint8_t *raw_data);

/**
 * Add a drop batch to a map.
 *
 * \param map The map to add the batch to
 * \param char_id The ID of the batch owner. If it was dropped by a non-player map object then only this character or any of its party members will be able to pick up the drop for a limited amount of time
 * \param oid The object ID the object that dropped it, or -1 if it was dropped by a player
 * \param count The size of drops
 * \param drops An array of the drops to drop
 */
void map_add_drop_batch(struct Map *map, uint32_t char_id, uint32_t oid, size_t count, struct Drop *drops);

/**
 * Get a drop
 *
 * \param map The map to get the drop from
 * \param oid The object ID of the requested drop
 *
 * \return The drop object or NULL if it wasn't found
 */
const struct Drop *map_get_drop(struct Map *map, uint32_t oid);

/**
 * Remove a drop from a map.
 * The drop must exist. Use \p map_get_drop to check if the drop exists before removing it
 *
 * \param The map to remove the drop from
 * \param char_id the ID of the character that requested the remove
 * \param oid the object ID of the drop
 * \param[out] drop Detailed about the drop that was removed
 *
 * \return false if removal was unsuccessful, for example, oid doesn't refer to a drop or the given char_id isn't allowed to pick it up; true if it was successful
 */
void map_remove_drop(struct Map *map, uint32_t char_id, uint32_t oid);

#endif


#ifndef MAP_H
#define MAP_H

#include <stdint.h>

#include "drop.h"
#include "life.h"
//#include "server.h" // This is temporary for map_join, I should find a better way to do this
#include "../character.h"

struct Map;

struct MapPlayer;
struct MapMonster;
struct DropBatch;

/**
 * Creates a new map.
 *
 * \param room The room that this map is associated with
 *
 * \return The newly created map or NULL if an error occurred
 */
struct Map *map_create(uint32_t id);

/**
 * Destroys a map
 *
 * \param map The map to destroy
 */
void map_destroy(struct Map *map);

bool map_respawn(struct Map *map, struct MapPlayer **controller, size_t *count, struct MapMonster ***monsters);

struct MapPlayer *map_next_controller(struct Map *map, struct MapPlayer *player);

const struct Player *map_player_get_player(struct MapPlayer *player);
void map_player_for_each_controlled_monster(struct MapPlayer *player, void (*)(struct MapMonster *monster, void *), void *);
bool map_player_is_seated(struct MapPlayer *player);
struct Point map_player_coords(struct MapPlayer *player);
void map_player_stand_up(struct MapPlayer *player);
uint16_t map_player_get_map_seat(struct MapPlayer *player);
void map_player_sit(struct MapPlayer *player, uint16_t id);
void map_player_chair(struct MapPlayer *player, uint32_t id);

uint32_t map_get_id(struct Map *map);

/**
 * Do an action for each drop in the map
 *
 * \param map The map
 * \param f The action to take
 * \param ctx Additional context to pass as the second argument to \p f
 */
void map_for_each_drop(struct Map *map, void (*f)(struct Drop *, void *), void *ctx);

void map_for_each_monster(struct Map *map, void (*f)(struct MapMonster *, void *), void *ctx);
void map_for_each_npc(struct Map *map, void (*f)(struct Npc *, void *), void *ctx);
void map_for_each_player(struct Map *map, void (*f)(struct MapPlayer *, void *), void *ctx);

void map_spawn(struct Map *map, uint32_t id, struct Point p);

struct MapMonster *map_get_monster(struct Map *map, uint32_t oid);

void drop_batch_for_each_drop(struct DropBatch *batch, void (*f)(struct Drop *drop, void *ctx), void *ctx);

/**
 * Kill all monsters on the map
 *
 * \param map The map
 * \param[out] count The number of monster killed
 *
 * \return An array of *count size of the IDs of the killed monsters
 */
void map_kill_all(struct Map *map);

const char *map_hit_reactor(struct Map *map, struct MapPlayer *player, uint32_t oid, uint32_t *id);

/**
 * Gets an NPC in the map
 *
 * \param map The map to get the NPC from.
 * \param oid The object ID of the NPC to get
 *
 * \return The NPC ID
 */
uint32_t map_get_npc(struct Map *map, uint32_t oid);

const struct Reactor *map_get_reactor(struct Map *map, uint32_t oid);

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
bool map_move_monster(struct Map *map, struct MapPlayer *controller, uint32_t oid, int16_t x, int16_t y, uint16_t fh, uint8_t stance);

int map_drop_batch_from_reactor(struct Map *map, struct MapPlayer *player, uint32_t oid);

void map_remove_all_drops(struct Map *map);

/**
 * Add a drop to a map
 *
 * \param map The map to add the drop to
 * \param char_id The ID of the dropper
 * \param drop The drop
 */
struct DropBatch *map_add_player_drop(struct Map *map, struct MapPlayer *player, struct Drop *drop);

/**
 * Get a drop
 *
 * \param map The map to get the drop from
 * \param oid The object ID of the requested drop
 *
 * \return The drop object or NULL if it wasn't found
 */
const struct Drop *map_get_drop(struct Map *map, uint32_t oid);

bool map_player_can_pick_up_drop(struct Map *map, struct MapPlayer *player, uint32_t oid);

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
void map_remove_drop(struct Map *map, uint32_t oid);

bool map_try_occupy_seat(struct Map *map, uint16_t id);
void map_tire_seat(struct Map *map, uint16_t id);

/**
 * Insert a new player to a map
 *
 * \param map The map to insert into
 * \param client The player to be inserted
 * \param[out] player A handle that respresents the player in the map
 *
 * \return NULL if an error occurred;
 */
struct MapPlayer *map_join(struct Map *map, uint32_t id, struct Player *player);

/**
 * Remove a player from a map
 *
 * \param map The map to remove from
 * \param player The handle that was returned in \p map_join of the player to remove
 */
void map_leave(struct Map *map, struct MapPlayer *player);

struct MapMonster *map_add_monster(struct Map *map, uint32_t id, int16_t x, int16_t y);
void map_remove_monster(struct Map *map, struct MapMonster *monster);

uint32_t map_player_id(struct MapPlayer *player);
const struct Point *map_player_pos(struct MapPlayer *player);
void map_player_update_stance(struct MapPlayer *player, uint8_t stance);
void map_player_update_pos(struct MapPlayer *player, uint16_t x, uint16_t y, uint16_t fh);

const struct Monster *map_monster_get_monster(struct MapMonster *monster);

bool map_monster_damage_by(struct MapMonster *monster, struct MapPlayer *player, size_t hit_count, int32_t *damage);

struct MapPlayer *map_monster_swap_controller(struct MapMonster *monster, struct MapPlayer *player);

void map_monster_remove_controller(struct MapMonster *monster);
/**
 * Get if the monster is alive
 *
 * \param map The map to check in
 * \param id The monster ID - used as an additional check if the oid still refers to the same object client-side
 * \param oid The monster's object ID
 *
 * \return true if the monster is still alive
 */
bool map_monster_is_alive(struct MapMonster *monster);

struct DropBatch;

struct DropBatch *map_add_drop_batch(struct Map *map, struct MapPlayer *owner, bool exclusive, uint32_t dropperOid, struct Point origin, size_t count, struct Drop *drops);

#endif


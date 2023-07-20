#ifndef ROOM_H
#define ROOM_H

#include <stdint.h>

#include "../hash-map.h"
#include "scripting/script-manager.h"
#include "session.h"
#include "drop.h"
#include "event-manager.h"
#include "thread-pool.h"
#include "player.h"

struct Room;
struct RoomMember;

struct Room *room_create(struct Worker *worker, struct EventManager *mgr, uint32_t id);
void room_destroy(struct EventManager *mgr, struct Room *room);
struct RoomMember *room_join(struct Room *room, struct Session *session, struct Player *player, struct HashSetU32 *quest_items, struct ScriptManager *reactor_manager);
void room_leave(struct Room *room, struct RoomMember *member);
uint32_t room_id(struct Room *room);
bool room_keep_alive(struct Room *room);
void room_foreach_member(struct Room *room, void (*f)(struct RoomMember *, void *), void *ctx);
void room_broadcast_sit_on_map_seat(struct Room *room, uint16_t id);
void room_broadcast_stand_up(struct Room *room, uint16_t id);
bool room_monster_exists(struct Room *room, uint32_t oid, uint32_t id);
uint32_t room_get_npc(struct Room *room, uint32_t oid);

void room_member_broadcast(struct Room *room, struct RoomMember *member, size_t len, uint8_t *packet);
bool room_member_close_range_attack(struct Room *room, struct RoomMember *member, uint8_t monster_count, uint8_t hit_count, uint32_t skill, uint8_t level, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t *oids, int32_t *damage, size_t *count, uint32_t *ids);
bool room_member_ranged_attack(struct Room *room, struct RoomMember *member, uint8_t monster_count, uint8_t hit_count, uint32_t skill, uint8_t level, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t *oids, int32_t *damage, uint32_t projectile, size_t *count, uint32_t *ids);
bool room_member_magic_attack(struct Room *room, struct RoomMember *member, uint8_t monster_count, uint8_t hit_count, uint32_t skill, uint8_t level, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t *oids, int32_t *damage, size_t *count, uint32_t *ids);
void room_member_update_stance(struct RoomMember *member, uint8_t stance);
void room_member_update_coords(struct RoomMember *member, int16_t x, int16_t y, uint16_t fh);
void room_member_move(struct Room *room, struct RoomMember *member, size_t len, uint8_t *packet);
bool room_member_damage_monster(struct Room *room, struct RoomMember *member, uint32_t oid, uint8_t hit_count, int32_t *damage, uint32_t *id);
int room_member_sit_packet(struct Room *room, struct RoomMember *member, uint16_t id);
int room_member_chair(struct Room *room, struct RoomMember *member, uint32_t chair);
void room_member_pick_up_drop(struct Room *room, struct RoomMember *member, uint32_t oid);
void room_member_level_up(struct Room *room, struct RoomMember *member);
void room_member_effect(struct Room *room, struct RoomMember *member, uint8_t effect);
void room_member_take_damage(struct Room *room, struct RoomMember *member, uint8_t skill, int32_t damage, uint32_t id, uint8_t direction);
void room_member_chat(struct Room *room, struct RoomMember *member, size_t len, char *string, uint8_t show);
void room_member_emote(struct Room *room, struct RoomMember *member, uint32_t emote);
bool room_member_drop(struct Room *room, struct RoomMember *member, struct Drop *drop);
bool room_member_add_quest_items(struct RoomMember *member, size_t count, uint32_t *ids);
bool room_member_move_monster(struct Room *room, struct RoomMember *member, uint32_t oid, uint16_t moveid, uint8_t activity, uint8_t skill_id, uint8_t skill_level, uint16_t option, size_t len, uint8_t *packet, int16_t x, int16_t y, uint16_t fh, uint8_t stance);
bool room_member_get_drop(struct Room *room, struct RoomMember *member, uint32_t oid, struct Drop *drop);
void room_member_hit_reactor(struct Room *room, struct RoomMember *member, uint32_t oid, uint8_t stance);

#endif


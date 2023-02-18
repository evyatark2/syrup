#include "packet.h"

#include <assert.h>
#include <string.h>

#include "writer.h"
#include "hash-map.h"

size_t login_success_packet(uint32_t id, uint8_t gender, uint8_t name_len, char *name, enum PicStatus pic, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, 42 + name_len, packet);
    // Opcode
    writer_u16(&writer, 0x0000);

    writer_zero(&writer, 6);

    // Account ID
    writer_u32(&writer, id);

    // Gender
    writer_u8(&writer, gender);

    // Flying
    writer_u8(&writer, 0);
    writer_u8(&writer, 0); // Admin byte

    // Coutnry code
    writer_u8(&writer, 0);

    writer_sized_string(&writer, name_len, name);

    writer_u8(&writer, 0);

    writer_u8(&writer, 0);  // IsQuietBan
    writer_u64(&writer, 0); // IsQuietBanTimeStamp
    writer_u64(&writer, 0); // CreationTimeStamp

    writer_u32(&writer, 1); // Removes the "Select the world you wnat to play in" pop up

    // Pin
    writer_u8(&writer, 1); // 0 - enabled; 1 - diabled
    // Pic
    writer_u8(&writer, pic); // 0 - register; 1 - ask; 2 - disabled

    return 42 + name_len;
}

void login_failure_packet(enum LoginFailureReason reason, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, LOGIN_FAILURE_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0000);
    writer_u16(&writer, reason);
    writer_u32(&writer, 0);
}

void pin_packet(enum PinPacketMode mode, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, PIN_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0006);
    writer_u8(&writer, mode);
}

char *WORLD_NAMES[] = {"Scania", "Bera"};

#define STRINGIFY(x) #x

size_t server_list_packet(uint8_t world, enum WorldFlag flag, uint16_t mes_len, char *mes, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SERVER_LIST_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x000A);
    writer_u8(&writer, world);
    writer_sized_string(&writer, strlen(WORLD_NAMES[world]), WORLD_NAMES[world]);
    writer_u8(&writer, flag);
    writer_sized_string(&writer, mes_len, mes);
    writer_u8(&writer, 100); // Rate modifier
    writer_u8(&writer, 0);   // XP * 2.6
    writer_u8(&writer, 100); // Rate modifier
    writer_u8(&writer, 0);   // drop * 2.6
    writer_u8(&writer, 0);

    writer_u8(&writer, 2); // Channel count

    writer_sized_string(&writer, 1, STRINGIFY(0));
    writer_u32(&writer, 0); // channel load 0~800
    writer_u8(&writer, world);
    writer_u8(&writer, 0);
    writer_bool(&writer, false);

    writer_sized_string(&writer, 1, STRINGIFY(1));
    writer_u32(&writer, 0); // channel load 0~800
    writer_u8(&writer, world);
    writer_u8(&writer, 1);
    writer_bool(&writer, false);

    writer_u16(&writer, 0);

    return writer.pos;
}

void server_list_end_packet(uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SERVER_LIST_END_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x000A);
    writer_u8(&writer, 0xFF);
}

void server_status_packet(enum ServerStatus status, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SERVER_STATUS_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0003);
    writer_u16(&writer, status);
}

size_t character_list_packet(enum LoginFailureReason status, uint8_t char_count, struct CharacterStats *chars, uint8_t pic, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CHARACTER_LIST_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x000B);
    writer_u8(&writer, status);
    writer_u8(&writer, char_count);
    for (uint8_t i = 0; i < char_count; i++) {
        writer_char_stats(&writer, chars + i);
        writer_char_appearance(&writer, &chars[i].info.appearance, false);
        writer_u8(&writer, 0); // NOT a view-all
        writer_u8(&writer, 0); // World ranking disabled; When enabled, additional ranking data needs to be supplied
    }

    writer_u8(&writer, pic);
    writer_u32(&writer, 6); // Total character slots; should be greater or equal to char_count

    return writer.pos;
}

void channel_ip_packet(uint32_t addr, uint16_t port, uint32_t token, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CHANNEL_IP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x000C);
    writer_u16(&writer, 0);
    writer_u32(&writer, addr);
    writer_u16(&writer, port);
    writer_u32(&writer, token);
    writer_zero(&writer, 5);
}

void login_error_packet(uint8_t err, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, LOGIN_ERROR_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0009);
    writer_u16(&writer, err);
}

size_t name_check_response_packet(uint8_t name_len, char *name, bool available, uint8_t *packet) {
    struct Writer writer;
    writer_init(&writer, NAME_CHECK_RESPONSE_PACKET_MAX_LENGTH, packet);
    writer_u16(&writer, 0x000D);
    writer_sized_string(&writer, name_len, name);
    writer_u8(&writer, available ? 0 : 1);
    return writer.pos;
}

size_t create_character_response_packet(struct CharacterStats *chr, uint8_t *packet) {
    struct Writer writer;
    writer_init(&writer, CREATE_CHARACTER_RESPONSE_PACKET_LENGTH, packet);
    writer_u16(&writer, 0x000E);
    writer_u8(&writer, 0);
    writer_char_stats(&writer, chr);
    writer_char_appearance(&writer, &chr->info.appearance, false);
    writer_u8(&writer, 0); // NOT a view-all
    writer_u8(&writer, 0); // World ranking disabled; When enabled additional ranking data needs to be supplied
    return writer.pos;
}

static void add_skills(void *data, void *ctx);
static void add_quests(void *data, void *ctx_);
static void add_quest_infos(void *data, void *ctx_);
static void add_completed_quests(void *data, void *ctx_);
static void add_monster_book_entries(void *data, void *ctx);

size_t enter_map_packet(struct Character *chr, uint8_t *packet) {
    struct Writer writer;
    writer_init(&writer, ENTER_MAP_PACKET_MAX_LENGTH, packet);
    writer_u16(&writer, 0x007D);
    writer_u32(&writer, 0); // Channel
    writer_u8(&writer, 1);
    writer_u8(&writer, 1);
    writer_u16(&writer, 0);

    // Three random ints?
    writer_u32(&writer, 0);
    writer_u32(&writer, 0);
    writer_u32(&writer, 0);

    writer_i64(&writer, -1);
    writer_u8(&writer, 0);
    struct CharacterStats stats = character_to_character_stats(chr);
    writer_char_stats(&writer, &stats);

    writer_u8(&writer, 20); // Buddy list capacity

    writer_u8(&writer, 0); // Linked name

    writer_i32(&writer, chr->mesos);

    // Inventory limits
    writer_u8(&writer, chr->equipmentInventory.slotCount);

    for (uint8_t i = 0; i < 3; i++)
        writer_u8(&writer, chr->inventory[i].slotCount);

    writer_u8(&writer, 252); // Max cash limit

    writer_u64(&writer, 94354848000000000L); // ZERO_TIME

    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (chr->equippedEquipment[i].isEmpty)
            continue;

        writer_u16(&writer, equip_slot_from_compact(i));
        writer_u8(&writer, 1); // Item type
        writer_u32(&writer, chr->equippedEquipment[i].equip.item.itemId);
        writer_bool(&writer, false); // Is cash? if it is, an additional cash id needs to be supplied
        writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME
        writer_u8(&writer, chr->equippedEquipment[i].equip.slots);
        writer_u8(&writer, 0); // level (the number of times a scroll was successfuly applied)
        writer_i16(&writer, chr->equippedEquipment[i].equip.str);
        writer_i16(&writer, chr->equippedEquipment[i].equip.dex);
        writer_i16(&writer, chr->equippedEquipment[i].equip.int_);
        writer_i16(&writer, chr->equippedEquipment[i].equip.luk);
        writer_i16(&writer, chr->equippedEquipment[i].equip.hp);
        writer_i16(&writer, chr->equippedEquipment[i].equip.mp);
        writer_i16(&writer, chr->equippedEquipment[i].equip.atk);
        writer_i16(&writer, chr->equippedEquipment[i].equip.matk);
        writer_i16(&writer, chr->equippedEquipment[i].equip.def);
        writer_i16(&writer, chr->equippedEquipment[i].equip.mdef);
        writer_i16(&writer, chr->equippedEquipment[i].equip.acc);
        writer_i16(&writer, chr->equippedEquipment[i].equip.avoid);
        writer_i16(&writer, chr->equippedEquipment[i].equip.hands);
        writer_i16(&writer, chr->equippedEquipment[i].equip.speed);
        writer_i16(&writer, chr->equippedEquipment[i].equip.jump);
        writer_sized_string(&writer, chr->equippedEquipment[i].equip.item.ownerLength, chr->equippedEquipment[i].equip.item.owner);
        writer_i16(&writer, chr->equippedEquipment[i].equip.item.flags);
        writer_u8(&writer, 0);
        writer_u8(&writer, 1); // Item level
        writer_i32(&writer, 0); // exp
        writer_u32(&writer, 0); // vicious
        writer_u64(&writer, 0);
        writer_u64(&writer, 94354848000000000L); // ZERO_TIME
        writer_i32(&writer, -1);
    }

    writer_u16(&writer, 0); // End of equipped equipment
    writer_u16(&writer, 0); // End of cosmetic equipped equipment

    for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        if (chr->equipmentInventory.items[i].isEmpty)
            continue;

        writer_u16(&writer, i + 1);
        writer_u8(&writer, 1); // Item type
        writer_u32(&writer, chr->equipmentInventory.items[i].equip.item.itemId);
        writer_bool(&writer, false); // Is cash? if it is, an additional cash id needs to be supplied
        writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME
        writer_u8(&writer, chr->equipmentInventory.items[i].equip.slots);
        writer_u8(&writer, 0); // level (the number of times a scroll was successfuly applied)
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.str);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.dex);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.int_);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.luk);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.hp);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.mp);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.atk);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.matk);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.def);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.mdef);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.acc);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.avoid);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.hands);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.speed);
        writer_i16(&writer, chr->equipmentInventory.items[i].equip.jump);
        writer_sized_string(&writer, chr->equipmentInventory.items[i].equip.item.ownerLength, chr->equipmentInventory.items[i].equip.item.owner);
        writer_u16(&writer, chr->equipmentInventory.items[i].equip.item.flags);
        writer_u8(&writer, 0);
        writer_u8(&writer, 1); // Item level
        writer_i32(&writer, 0); // exp
        writer_u32(&writer, 0); // vicious
        writer_u64(&writer, 0);
        writer_u64(&writer, 94354848000000000L); // ZERO_TIME
        writer_i32(&writer, -1);
    }

    writer_u32(&writer, 0); // End of equipment inventory

    for (uint8_t i = 0; i < chr->inventory[0].slotCount; i++) {
        if (chr->inventory[0].items[i].isEmpty)
            continue;

        writer_u8(&writer, i + 1);
        writer_u8(&writer, 2); // Item type
        writer_u32(&writer, chr->inventory[0].items[i].item.item.itemId);
        writer_bool(&writer, false); // Is cash? if it is, an additional cash id needs to be supplied
        writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME
        writer_i16(&writer, chr->inventory[0].items[i].item.quantity);
        writer_sized_string(&writer, chr->inventory[0].items[i].item.item.ownerLength, chr->inventory[0].items[i].item.item.owner);
        writer_u16(&writer, chr->inventory[0].items[i].item.item.flags);
        if (chr->inventory[0].items[i].item.item.itemId / 10000 == 207 || chr->inventory[0].items[i].item.item.itemId / 10000 == 233) {
            writer_u32(&writer, 2);
            writer_array(&writer, 4, (uint8_t[]) { 0x54, 0x00, 0x00, 0x34 });
        }
    }

    writer_u8(&writer, 0);  // End of use inventory

    for (uint8_t i = 0; i < chr->inventory[1].slotCount; i++) {
        if (chr->inventory[1].items[i].isEmpty)
            continue;

        writer_u8(&writer, i + 1);
        writer_u8(&writer, 2); // Item type
        writer_u32(&writer, chr->inventory[1].items[i].item.item.itemId);
        writer_bool(&writer, false); // Is cash? if it is, an additional cash id needs to be supplied
        writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME
        writer_i16(&writer, chr->inventory[1].items[i].item.quantity);
        writer_sized_string(&writer, chr->inventory[1].items[i].item.item.ownerLength, chr->inventory[1].items[i].item.item.owner);
        writer_u16(&writer, chr->inventory[1].items[i].item.item.flags);
    }

    writer_u8(&writer, 0);  // End of setup inventory

    for (uint8_t i = 0; i < chr->inventory[2].slotCount; i++) {
        if (chr->inventory[2].items[i].isEmpty)
            continue;

        writer_u8(&writer, i + 1);
        writer_u8(&writer, 2); // Item type
        writer_u32(&writer, chr->inventory[2].items[i].item.item.itemId);
        writer_bool(&writer, false); // Is cash? if it is, an additional cash id needs to be supplied
        writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME
        writer_i16(&writer, chr->inventory[2].items[i].item.quantity);
        writer_sized_string(&writer, chr->inventory[2].items[i].item.item.ownerLength, chr->inventory[2].items[i].item.item.owner);
        writer_u16(&writer, chr->inventory[2].items[i].item.item.flags);
    }

    writer_u8(&writer, 0);  // End of etc inventory
    writer_u8(&writer, 0);  // End of cash inventory

    // Skills
    writer_u16(&writer, hash_set_u32_size(chr->skills)); // Number for skills
    hash_set_u32_foreach(chr->skills, add_skills, &writer);

    writer_u16(&writer, 0); // Number for cooldowns

    // Quests
    writer_i16(&writer, hash_set_u16_size(chr->quests) + hash_set_u16_size(chr->questInfos)); // Started quests count
    hash_set_u16_foreach(chr->quests, add_quests, &writer);
    hash_set_u16_foreach(chr->questInfos, add_quest_infos, &writer);

    writer_i16(&writer, hash_set_u16_size(chr->completedQuests)); // Completed quests count
    hash_set_u16_foreach(chr->completedQuests, add_completed_quests, &writer);

    writer_u16(&writer, 0); // Mini game info
    writer_u16(&writer, 0); // Crush rings count
    writer_u16(&writer, 0); // Friendship rings count
    writer_u16(&writer, 0); // 0 - No partner, 1 - Partner exists (additional information about them needs to be provided)

    // Teleport rock locations
    for (int i = 0; i < 5; i++)
        writer_u32(&writer, 999999999);

    // VIP teleport rock locations
    for (int i = 0; i < 10; i++)
        writer_u32(&writer, 999999999);

    // Monster book
    writer_u32(&writer, 0); // Cover
    writer_u8(&writer, 0);
    writer_u16(&writer, hash_set_u32_size(chr->monsterBook));
    hash_set_u32_foreach(chr->monsterBook, add_monster_book_entries, &writer);

    writer_u16(&writer, 0); // New year records count

    writer_u16(&writer, 0); // Area info count

    writer_u16(&writer, 0); // End
    struct timeval time;
    gettimeofday(&time, NULL);
    struct tm now;
    localtime_r(&time.tv_sec, &now);
    writer_u64(&writer, (time.tv_sec * 1000L + time.tv_usec / 1000L) * 10000L + 116444736010800000L + now.tm_gmtoff * 1000L * 10000L); // Current time
    return writer.pos;
}

void set_gender_packet(bool gender, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CHANGE_MAP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x003A);
    writer_bool(&writer, gender);
}

void change_map_packet(struct Character *chr, uint32_t to, uint8_t portal, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CHANGE_MAP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x007D);
    writer_u32(&writer, 0); // Channel
    writer_u32(&writer, 0);
    writer_u8(&writer, 0);
    writer_u32(&writer, to);
    writer_u8(&writer, portal);
    writer_u16(&writer, chr->hp);
    writer_bool(&writer, false);
    struct timeval time;
    gettimeofday(&time, NULL);
    struct tm now;
    localtime_r(&time.tv_sec, &now);
    writer_u64(&writer, (time.tv_sec * 1000L + time.tv_usec / 1000L) * 10000L + 116444736010800000L + now.tm_gmtoff * 1000L * 10000L); // Current time
}

size_t add_player_to_map_packet(const struct Character *chr, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x00A0);
    writer_u32(&writer, chr->id);
    writer_u8(&writer, chr->level);
    writer_sized_string(&writer, chr->nameLength, chr->name);
    writer_zero(&writer, 8); // Guild

    // Foreign buffs
    writer_u32(&writer, 0);
    writer_u16(&writer, 0);
    writer_u8(&writer, 0xFC);
    writer_u8(&writer, 1);
    writer_u32(&writer, 0); // Morph
    writer_u32(&writer, 0); // High bytes of buff mask
    writer_u32(&writer, 0); // Low bytes of buff mask

    // Energy charge
    writer_u32(&writer, 0);
    writer_u16(&writer, 0);
    writer_u32(&writer, 0);

    // Dash Speed
    writer_u32(&writer, 0);
    writer_zero(&writer, 11);
    writer_u16(&writer, 0);

    // Dash Jump
    writer_zero(&writer, 9);
    writer_u32(&writer, 0);
    writer_u16(&writer, 0);
    writer_u8(&writer, 0);

    // Monster Riding
    writer_u64(&writer, 0);

    writer_u32(&writer, 0);
    writer_zero(&writer, 8);
    writer_u32(&writer, 0);
    writer_u8(&writer, 0);
    writer_u32(&writer, 0);
    writer_u16(&writer, 0);
    writer_zero(&writer, 9);
    writer_u32(&writer, 0);
    writer_u32(&writer, 0);
    writer_zero(&writer, 9);
    writer_u32(&writer, 0);
    writer_u32(&writer, 0);

    writer_u16(&writer, chr->job);

    struct CharacterAppearance appearance = character_to_character_appearance(chr);
    writer_char_appearance(&writer, &appearance, false);
    writer_u32(&writer, 0); // Number of heart-shaped chocolates
    writer_u32(&writer, 0); // Item effect
    writer_u32(&writer, 0); // Chair
    writer_i16(&writer, chr->x);
    writer_i16(&writer, chr->y);
    writer_u8(&writer, chr->stance);
    writer_i16(&writer, chr->fh); // Fh
    writer_u8(&writer, 0);
    writer_u8(&writer, 0); // End of pets

    // Mob
    writer_u32(&writer, 1);
    writer_u32(&writer, 0);
    writer_u32(&writer, 0);

    writer_u8(&writer, 0); // Shop or MiniGame

    writer_u8(&writer, 0); // Chalkboard

    writer_u8(&writer, 0); // Crush rings
    writer_u8(&writer, 0); // Friendship rings
    writer_u8(&writer, 0); // Marriage ring
    writer_u8(&writer, 0); // New years card

    writer_u16(&writer, 0);
    writer_u8(&writer, 0); // Team (used in events)

    return writer.pos;
}

size_t move_player_packet(uint32_t id, size_t len, uint8_t *data, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, 10 + len, packet);

    writer_u16(&writer, 0x00B9);
    writer_u32(&writer, id);
    writer_u32(&writer, 0);
    writer_array(&writer, len, data);

    return 10 + len;
}

size_t damange_player_packet(uint8_t skill, uint32_t monster_id, uint32_t char_id, int32_t damage, int32_t fake, uint8_t direction, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, DAMAGE_PLAYER_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x00C0);
    writer_u32(&writer, char_id);
    writer_u8(&writer, skill);
    writer_i32(&writer, damage);
    if (skill != (uint8_t)-4) {
        writer_u32(&writer, monster_id);
        writer_u8(&writer, direction);
        writer_u16(&writer, 0);
        writer_i32(&writer, damage);
        if (fake > 0)
            writer_i32(&writer, fake);
    } else {
        writer_i32(&writer, damage);
    }

    return writer.pos;
}

void remove_player_from_map_packet(uint32_t id, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00A1);
    writer_u32(&writer, id);
}

void spawn_npc_packet(uint32_t oid, uint32_t id, int16_t x, int16_t cy, bool f, uint16_t fh, int16_t rx0, int16_t rx1, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SPAWN_NPC_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0101);
    writer_u32(&writer, oid);
    writer_u32(&writer, id);
    writer_i16(&writer, x);
    writer_i16(&writer, cy);
    writer_u8(&writer, f ? 0 : 1);
    writer_u16(&writer, fh);
    writer_i16(&writer, rx0);
    writer_i16(&writer, rx1);
    writer_u8(&writer, 1);
}

void spawn_npc_controller_packet(uint32_t oid, uint32_t id, int16_t x, int16_t cy, bool f, uint16_t fh, int16_t rx0, int16_t rx1, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SPAWN_NPC_CONTROLLER_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0103);
    writer_u8(&writer, 1);
    writer_u32(&writer, oid);
    writer_u32(&writer, id);
    writer_i16(&writer, x);
    writer_i16(&writer, cy);
    writer_u8(&writer, f ? 0 : 1);
    writer_u16(&writer, fh);
    writer_i16(&writer, rx0);
    writer_i16(&writer, rx1);
    writer_bool(&writer, true); // MiniMap - doesn't look like it is ever called with 'false'
}

void spawn_monster_packet(uint32_t oid, uint32_t id, int16_t x, int16_t y, uint16_t fh, bool new, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SPAWN_MONSTER_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00EC);
    writer_u32(&writer, oid);
    writer_u8(&writer, 5); // No controller
    writer_u32(&writer, id);
    writer_zero(&writer, 16);
    writer_i16(&writer, x);
    writer_i16(&writer, y);
    writer_u8(&writer, 5); // Stance
    writer_i16(&writer, 0); // Origin FH
    writer_u16(&writer, fh);
    writer_i8(&writer, new ? -2 : -1);
    writer_i8(&writer, -1); // Team - used for monster carnival and other events
    writer_u32(&writer, 0);
}

void spawn_monster_controller_packet(uint32_t oid, bool aggro, uint32_t id, int16_t x, int16_t y, uint16_t fh, bool new, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SPAWN_MONSTER_CONTROLLER_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00EE);
    writer_u8(&writer, aggro ? 2 : 1);
    writer_u32(&writer, oid);
    writer_u8(&writer, 1); // Has controller
    writer_u32(&writer, id);
    writer_zero(&writer, 16); // Status effects
    writer_i16(&writer, x);
    writer_i16(&writer, y);
    writer_u8(&writer, 5); // Stance
    writer_i16(&writer, 0); // Origin FH
    writer_u16(&writer, fh);
    writer_i8(&writer, new ? -2 : -1);
    writer_i8(&writer, -1); // Team - used for monster carnival and other events
    writer_u32(&writer, 0);
}

void remove_monster_controller_packet(uint32_t oid, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, REMOVE_MONSTER_CONTROLLER_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00EE);
    writer_u8(&writer, 0);
    writer_u32(&writer, oid);
}

void spawn_reactor_packet(uint32_t oid, uint32_t id, int16_t x, int16_t y, uint8_t state, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SPAWN_REACTOR_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0117);
    writer_u32(&writer, oid);
    writer_u32(&writer, id);
    writer_u8(&writer, state);
    writer_i16(&writer, x);
    writer_i16(&writer, y);
    writer_u8(&writer, 0);
    writer_u16(&writer, 0);
}

void change_reactor_state_packet(uint32_t oid, uint8_t state, int16_t x, int16_t y, uint8_t stance, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CHANGE_REACTOR_STATE_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0115);
    writer_u32(&writer, oid);
    writer_u8(&writer, state);
    writer_i16(&writer, x);
    writer_i16(&writer, y);
    writer_u8(&writer, stance);
    writer_u16(&writer, 0);
    writer_u8(&writer, 5);
}

void destroy_reactor_packet(uint32_t oid, uint8_t state, int16_t x, int16_t y, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, DESTROY_REACTOR_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0118);
    writer_u32(&writer, oid);
    writer_u8(&writer, state);
    writer_i16(&writer, x);
    writer_i16(&writer, y);
}

void npc_action_packet(size_t size, uint8_t *data, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, 2 + size, packet);

    writer_u16(&writer, 0x0104);
    writer_array(&writer, size, data);
}

size_t move_monster_packet(uint32_t oid, uint8_t activity, size_t len, uint8_t *data, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, MOVE_MOB_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x00EF);
    writer_u32(&writer, oid);
    writer_u8(&writer, 0);
    writer_u8(&writer, 0);
    writer_u8(&writer, activity);
    writer_u8(&writer, 0);
    writer_u8(&writer, 0);
    writer_u16(&writer, 0);
    writer_array(&writer, len, data);

    return writer.pos;
}

void move_monster_response_packet(uint32_t oid, uint16_t moveid, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, MOVE_MOB_RESPONSE_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00F0);
    writer_u32(&writer, oid);
    writer_u16(&writer, moveid);
    writer_bool(&writer, false);
    writer_i16(&writer, 5);
    writer_u8(&writer, 0);
    writer_u8(&writer, 0);
}

size_t close_range_attack_packet(uint32_t id, uint8_t skill, uint8_t skill_level, uint8_t monster_count, uint8_t hit_count, uint32_t *oids, int32_t *damage, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x00BA);
    writer_u32(&writer, id);
    uint8_t count = (monster_count << 4) | hit_count;
    writer_u8(&writer, count);
    writer_u8(&writer, 0x5B);
    writer_u8(&writer, skill_level);
    if (skill_level > 0)
        writer_u8(&writer, skill);

    writer_u8(&writer, display);
    writer_u8(&writer, direction);
    writer_u8(&writer, stance);
    writer_u8(&writer, speed);
    writer_u8(&writer, 0x0A);
    writer_u32(&writer, 0); // Projectile ID
    for (uint8_t i = 0; i < monster_count; i++) {
        writer_u32(&writer, oids[i]);
        writer_u8(&writer, 0);
        for (uint8_t j = 0; j < hit_count; j++)
            writer_i32(&writer, damage[i * hit_count + j]);
    }

    return writer.pos;
}

size_t ranged_attack_packet(uint32_t id, uint8_t skill, uint8_t skill_level, uint8_t monster_count, uint8_t hit_count, uint32_t *oids, int32_t *damage, uint8_t display, uint8_t direction, uint8_t stance, uint8_t speed, uint32_t projectile, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, RANGED_ATTACK_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x00BB);
    writer_u32(&writer, id);
    uint8_t count = (monster_count << 4) | hit_count;
    writer_u8(&writer, count);
    writer_u8(&writer, 0x5B);
    writer_u8(&writer, skill_level);
    if (skill_level > 0)
        writer_u32(&writer, skill);

    writer_u8(&writer, display);
    writer_u8(&writer, direction);
    writer_u8(&writer, stance);
    writer_u8(&writer, speed);
    writer_u8(&writer, 0x0A);
    writer_u32(&writer, projectile);
    for (uint8_t i = 0; i < monster_count; i++) {
        writer_u32(&writer, oids[i]);
        writer_u8(&writer, 0);
        for (uint8_t j = 0; j < hit_count; j++)
            writer_i32(&writer, damage[i * hit_count + j]);
    }

    writer_u32(&writer, 0);

    return writer.pos;
}

void monster_hp_packet(uint32_t oid, uint8_t hp, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CLOSE_RANGE_ATTACK_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x00FA);
    writer_u32(&writer, oid);
    writer_u8(&writer, hp);
}

void kill_monster_packet(uint32_t oid, bool animation, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, MOVE_MOB_RESPONSE_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00ED);
    writer_u32(&writer, oid);
    writer_bool(&writer, animation);
    writer_bool(&writer, animation);
}

size_t open_shop_packet(uint32_t id, uint16_t item_count, struct ShopItem *items, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, OPEN_SHOP_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x0131);
    writer_u32(&writer, id);
    writer_u16(&writer, item_count);
    for (uint16_t i = 0; i < item_count; i++) {
        writer_u32(&writer, items[i].id);
        writer_i32(&writer, items[i].price);
        writer_u32(&writer, 0);
        writer_u32(&writer, 0);
        writer_u32(&writer, 0);
        if (items[i].id / 10000 != 207 && items[i].id / 10000 != 233) {
            writer_u16(&writer, 1);
            writer_u16(&writer, 1000); // Max buyable stack size
        } else {
            writer_u16(&writer, 0);
            writer_u32(&writer, 0);
            const struct ItemInfo *info = wz_get_item_info(items[i].id);
            const uint64_t *rep = &info->unitPrice; // Assuming a IEEE-754 representation
            writer_u16(&writer, *rep >> 48);
            writer_u16(&writer, info->slotMax); // TODO: Should also take into account the player's extra slots
        }
    }

    return writer.pos;
}

void shop_action_response(uint8_t code, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0132);
    writer_u8(&writer, code);
}

size_t stat_change_packet(bool enable_actions, enum Stat stats, union StatValue *values, uint8_t *packet)
{
    // Count the number of stat changes
    uint8_t count = 0;
    uint32_t s = stats;
    if (s & 0x180008) {
        s &= ~(0x180008);
        count++;
    }

    for (; s; count++) {
        s &= s - 1;
    }

    struct Writer writer;
    writer_init(&writer, STAT_CHANGE_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x001F);
    writer_bool(&writer, enable_actions);

    writer_u32(&writer, stats);
    for (uint8_t i = 0; i < count; i++) {
        if (stats & STAT_SKIN) {
            writer_u8(&writer, values[i].u8);
            stats &= ~STAT_SKIN;
        } else if (stats & STAT_FACE) {
            writer_u32(&writer, values[i].u32);
            stats &= ~STAT_FACE;
        } else if (stats & STAT_HAIR) {
            writer_u32(&writer, values[i].u32);
            stats &= ~STAT_HAIR;
        } else if (stats & STAT_LEVEL) {
            writer_u8(&writer, values[i].u8);
            stats &= ~STAT_LEVEL;
        } else if (stats & STAT_JOB) {
            writer_i16(&writer, values[i].u16);
            stats &= ~STAT_JOB;
        } else if (stats & STAT_STR) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_STR;
        } else if (stats & STAT_DEX) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_DEX;
        } else if (stats & STAT_INT) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_INT;
        } else if (stats & STAT_LUK) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_LUK;
        } else if (stats & STAT_HP) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_HP;
        } else if (stats & STAT_MAX_HP) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_MAX_HP;
        } else if (stats & STAT_MP) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_MP;
        } else if (stats & STAT_MAX_MP) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_MAX_MP;
        } else if (stats & STAT_AP) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_AP;
        } else if (stats & STAT_SP) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_SP;
        } else if (stats & STAT_EXP) {
            writer_i32(&writer, values[i].i32);
            stats &= ~STAT_EXP;
        } else if (stats & STAT_FAME) {
            writer_i16(&writer, values[i].i16);
            stats &= ~STAT_FAME;
        } else if (stats & STAT_MESO) {
            writer_i32(&writer, values[i].i32);
            stats &= ~STAT_MESO;
        } else if (stats & STAT_PET) {
            writer_u32(&writer, values[i].u32);
            stats &= ~STAT_PET;
        } else if (stats & STAT_GACHA_EXP) {
            writer_i32(&writer, values[i].i32);
            stats &= ~STAT_GACHA_EXP;
        }
    }

    return writer.pos;
}

static void exp_gain_packet_internal(struct Writer *writer, int32_t exp, int32_t equip_bonus, int32_t party_bonus, bool white, bool in_chat);

void exp_gain_packet(int32_t exp, int32_t equip_bonus, int32_t party_bonus, bool white, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, EXP_GAIN_PACKET_LENGTH, packet);
    exp_gain_packet_internal(&writer, exp, equip_bonus, party_bonus, white, false);
}

void exp_gain_in_chat_packet(int32_t exp, int32_t equip_bonus, int32_t party_bonus, bool white, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, EXP_GAIN_IN_CHAT_PACKET_LENGTH, packet);
    exp_gain_packet_internal(&writer, exp, equip_bonus, party_bonus, white, true);
}

void show_effect_packet(uint8_t effect, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SHOW_EFFECT_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00CE);
    writer_u8(&writer, effect);
}

void show_foreign_effect_packet(uint32_t id, uint8_t effect, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00C6);
    writer_u32(&writer, id);
    writer_u8(&writer, effect);
}

void drop_item_from_object_packet(uint32_t oid, uint32_t item_id, uint32_t owner_id, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, uint32_t dropper_oid, bool player_drop, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, DROP_ITEM_FROM_OBJECT_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x010C);
    writer_u8(&writer, 1); // mod
    writer_u32(&writer, oid);
    writer_bool(&writer, false);
    writer_u32(&writer, item_id);
    writer_u32(&writer, owner_id);
    writer_u8(&writer, 2); // Free for all
    writer_i16(&writer, to_x);
    writer_i16(&writer, to_y);
    writer_u32(&writer, dropper_oid);
    writer_i16(&writer, from_x);
    writer_i16(&writer, from_y);
    writer_u16(&writer, 0); // fh?

    writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME

    writer_bool(&writer, !player_drop);
}

void spawn_item_drop_packet(uint32_t oid, uint32_t item_id, uint32_t owner_id, int16_t to_x, int16_t to_y, uint32_t dropper_oid, bool player_drop, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SPAWN_ITEM_DROP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x010C);
    writer_u8(&writer, 2);
    writer_u32(&writer, oid);
    writer_bool(&writer, false);
    writer_u32(&writer, item_id);
    writer_u32(&writer, owner_id);
    writer_u8(&writer, 2); // Free for all
    writer_i16(&writer, to_x);
    writer_i16(&writer, to_y);
    writer_u32(&writer, dropper_oid);

    writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME

    writer_bool(&writer, !player_drop);
}

void remove_drop_packet(uint32_t oid, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, REMOVE_DROP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x010D);
    writer_u8(&writer, 0); // Use '1' for no vanish animation
    writer_u32(&writer, oid);
}

void pickup_drop_packet(uint32_t oid, bool is_exploding, uint32_t char_id, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, PICKUP_DROP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x010D);
    writer_u8(&writer, is_exploding ? 4 : 2);
    writer_u32(&writer, oid);
    writer_u32(&writer, char_id);
}

void pet_pickup_drop_packet(uint32_t oid, bool is_exploding, uint32_t char_id, uint8_t pet, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, PET_PICKUP_DROP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x010D);
    writer_u8(&writer, is_exploding ? 4 : 2);
    writer_u32(&writer, oid);
    writer_u32(&writer, char_id);
    writer_u8(&writer, pet);
}

void drop_meso_from_object_packet(uint32_t oid, int32_t meso, uint32_t owner_id, int16_t from_x, int16_t from_y, int16_t to_x, int16_t to_y, uint32_t dropper_oid, bool player_drop, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, DROP_MESO_FROM_OBJECT_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x010C);
    writer_u8(&writer, 1);
    writer_u32(&writer, oid);
    writer_bool(&writer, true);
    writer_i32(&writer, meso);
    writer_u32(&writer, owner_id);
    writer_u8(&writer, 2); // Free for all
    writer_i16(&writer, to_x);
    writer_i16(&writer, to_y);
    writer_u32(&writer, dropper_oid);
    writer_i16(&writer, from_x);
    writer_i16(&writer, from_y);
    writer_u16(&writer, 0); // fh?
    writer_bool(&writer, !player_drop);
}

void spawn_meso_drop_packet(uint32_t oid, int32_t meso, uint32_t owner_id, int16_t x, int16_t y, uint32_t dropper_oid, bool player_drop, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, SPAWN_MESO_DROP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x010C);
    writer_u8(&writer, 2);
    writer_u32(&writer, oid);
    writer_bool(&writer, true);
    writer_i32(&writer, meso);
    writer_u32(&writer, owner_id);
    writer_u8(&writer, 2); // Free for all
    writer_i16(&writer, x);
    writer_i16(&writer, y);
    writer_u32(&writer, dropper_oid);
    writer_bool(&writer, !player_drop);
}

void meso_gain_packet(int32_t amount, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, MESO_GAIN_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u8(&writer, 0);
    writer_u16(&writer, 1);
    writer_i32(&writer, amount);
    writer_u16(&writer, 0);
}

void meso_gain_in_chat_packet(int32_t amount, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, MESO_GAIN_IN_CHAT_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u8(&writer, 5);
    writer_i32(&writer, amount);
    writer_u16(&writer, 0);
}

void item_gain_packet(uint32_t id, int32_t amount, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, ITEM_GAIN_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u16(&writer, 0);
    writer_u32(&writer, id);
    writer_i32(&writer, amount);
    writer_u64(&writer, 0);
}

void item_gain_in_chat_packet(uint32_t id, int32_t amount, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, ITEM_GAIN_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00CE);
    writer_u8(&writer, 3);
    writer_u8(&writer, 1);
    writer_u32(&writer, id);
    writer_i32(&writer, amount);
}

void item_unavailable_notification_packet(uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u8(&writer, 0);
    writer_u8(&writer, 0xFE);
    writer_u64(&writer, 0);
}

void inventory_full_notification_packet(uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, INVENTORY_FULL_NOTIFICATION_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u8(&writer, 0);
    writer_u8(&writer, 0xFF);
    writer_u64(&writer, 0);
}

void start_quest_packet(uint16_t qid, uint32_t npc, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, START_QUEST_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00D3);
    writer_u8(&writer, 8);
    writer_u16(&writer, qid);
    writer_u32(&writer, npc);
    writer_u32(&writer, 0);
}

void end_quest_packet(uint16_t qid, uint32_t npc, uint16_t next, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, START_QUEST_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00D3);
    writer_u8(&writer, 8);
    writer_u16(&writer, qid);
    writer_u32(&writer, npc);
    writer_u16(&writer, next);
}

size_t update_quest_packet(uint16_t id, uint16_t len, const char *progress, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, UPDATE_QUEST_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u8(&writer, 1);
    writer_u16(&writer, id);
    writer_u8(&writer, 1);
    writer_sized_string(&writer, len, progress);
    writer_zero(&writer, 5);

    return writer.pos;
}

void update_quest_completion_time_packet(uint16_t qid, time_t time, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u8(&writer, 1);
    writer_u16(&writer, qid);
    writer_u8(&writer, 2);

    struct tm tm;
    localtime_r(&time, &tm);
    writer_u64(&writer, (time * 1000L) * 10000L + 116444736010800000L + tm.tm_gmtoff * 1000L * 10000L); // Current time
}

void forfeit_quest_packet(uint16_t qid, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, FORFEIT_QUEST_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0027);
    writer_u8(&writer, 1);
    writer_u16(&writer, qid);
    writer_u8(&writer, 0);
}

size_t chat_packet(uint32_t id, bool gm, uint16_t len, char *string, uint8_t show, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, CHAT_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x00A2);
    writer_u32(&writer, id);
    writer_bool(&writer, gm);
    writer_sized_string(&writer, len, string);
    writer_u8(&writer, show);

    return writer.pos;
}

void face_expression_packet(uint32_t id, uint32_t emote, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, FACE_EXPRESSION_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x00C1);
    writer_u32(&writer, id);
    writer_u32(&writer, emote);
}

void add_card_packet(bool full, uint32_t id, int8_t count, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, ADD_CARD_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0053);
    writer_bool(&writer, !full);
    writer_u32(&writer, id);
    writer_i32(&writer, count);
}

size_t modify_items_packet(uint8_t mod_count, struct InventoryModify *mods, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, MODIFY_ITEMS_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x001D);
    writer_bool(&writer, true); // updateTick?
    writer_u8(&writer, mod_count);
    for (uint8_t i = 0; i < mod_count; i++) {
        writer_u8(&writer, mods[i].mode);
        writer_u8(&writer, mods[i].inventory);
        writer_i16(&writer, mods[i].slot);
        switch (mods[i].mode) {
        case INVENTORY_MODIFY_TYPE_ADD:
            if (mods[i].inventory != 1) {
                writer_u8(&writer, 2); // Item type
                writer_u32(&writer, mods[i].item.item.itemId);
                writer_bool(&writer, false); // Is cash
                writer_u64(&writer, 150842304000000000L); // Unlimited expiration time
                writer_u16(&writer, mods[i].item.quantity);
                writer_sized_string(&writer, mods[i].item.item.ownerLength, mods[i].item.item.owner);
                writer_u16(&writer, mods[i].item.item.flags);
                if (mods[i].item.item.itemId / 10000 == 207 || mods[i].item.item.itemId / 10000 == 233) {
                    writer_u32(&writer, 2);
                    writer_array(&writer, 4, (uint8_t[]) { 0x54, 0x00, 0x00, 0x34 });
                }
            } else {
                writer_u8(&writer, 1);
                writer_u32(&writer, mods[i].equip.item.itemId);
                writer_bool(&writer, mods[i].equip.cash);
                writer_u64(&writer, 150842304000000000L); // Unlimited expiration time
                writer_u8(&writer, mods[i].equip.slots);
                writer_u8(&writer, mods[i].equip.level);
                writer_i16(&writer, mods[i].equip.str);
                writer_i16(&writer, mods[i].equip.dex);
                writer_i16(&writer, mods[i].equip.int_);
                writer_i16(&writer, mods[i].equip.luk);
                writer_i16(&writer, mods[i].equip.hp);
                writer_i16(&writer, mods[i].equip.mp);
                writer_i16(&writer, mods[i].equip.atk);
                writer_i16(&writer, mods[i].equip.matk);
                writer_i16(&writer, mods[i].equip.def);
                writer_i16(&writer, mods[i].equip.mdef);
                writer_i16(&writer, mods[i].equip.acc);
                writer_i16(&writer, mods[i].equip.avoid);
                writer_i16(&writer, mods[i].equip.hands);
                writer_i16(&writer, mods[i].equip.speed);
                writer_i16(&writer, mods[i].equip.jump);
                writer_sized_string(&writer, mods[i].equip.item.ownerLength, mods[i].equip.item.owner);
                writer_u16(&writer, mods[i].equip.item.flags);
                if (mods[i].equip.cash) {
                    writer_array(&writer, 10, (uint8_t[]) { 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40 });
                } else {
                    writer_u8(&writer, 0);
                    writer_u8(&writer, 1); // Item level
                    writer_u32(&writer, 0); // Item EXP
                    writer_u32(&writer, 0); // Vicious
                    writer_u64(&writer, 0);
                }
                writer_u64(&writer, 94354848000000000L); // ZERO_TIME
                writer_i32(&writer, -1);
            }
        break;
        case INVENTORY_MODIFY_TYPE_MODIFY:
            assert(mods[i].inventory != 1); // Can't change an equipment's quantity
            writer_i16(&writer, mods[i].quantity);
        break;
        case INVENTORY_MODIFY_TYPE_MOVE:
            writer_i16(&writer, mods[i].dst);
            if (mods[i].slot < 0 || mods[i].dst < 0) {
                writer_u8(&writer, mods[i].slot < 0 ? 1 : 2);
            }
        break;
        case INVENTORY_MODIFY_TYPE_REMOVE:
            if (mods[i].slot < 0)
                writer_u8(&writer, 2);
        break;
        }
    }

    return writer.pos;
}

size_t npc_dialogue_packet(uint32_t npc, enum NpcDialogueType type, uint16_t message_len, const char *message, uint8_t speaker, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, NPC_DIALOGUE_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x0130);
    writer_u8(&writer, 4);
    writer_u32(&writer, npc);

    if (type == NPC_DIALOGUE_TYPE_NEXT || type == NPC_DIALOGUE_TYPE_PREV_NEXT || type == NPC_DIALOGUE_TYPE_PREV)
        writer_u8(&writer, 0);
    else
        writer_u8(&writer, type);

    writer_u8(&writer, speaker); // Speaker
    writer_sized_string(&writer, message_len, message);
    switch (type) {
    case NPC_DIALOGUE_TYPE_OK:
        writer_u8(&writer, 0);
        writer_u8(&writer, 0);
    break;
    case NPC_DIALOGUE_TYPE_NEXT:
        writer_u8(&writer, 0);
        writer_u8(&writer, 1);
    break;
    case NPC_DIALOGUE_TYPE_PREV_NEXT:
        writer_u8(&writer, 1);
        writer_u8(&writer, 1);
    break;
    case NPC_DIALOGUE_TYPE_PREV:
        writer_u8(&writer, 1);
        writer_u8(&writer, 0);
    break;
    default:
    break;
    }

    return writer.pos;
}

void update_skill_packet(uint32_t id, int8_t level, int8_t master_level, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, UPDATE_SKILL_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x0024);
    writer_u8(&writer, 1);
    writer_u16(&writer, 1);
    writer_u32(&writer, id);
    writer_i32(&writer, level);
    writer_i32(&writer, master_level);
    writer_u64(&writer, 150842304000000000L); // DEFAULT_TIME
    writer_u8(&writer, 4);
}

size_t popup_message_packet(uint16_t len, const char *message, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, POPUP_MESSAGE_PACKET_MAX_LENGTH, packet);

    writer_u16(&writer, 0x0044);
    writer_u8(&writer, 1);
    writer_sized_string(&writer, len, message);

    return writer.pos;
}

void keymap_packet(const struct KeyMapEntry *keymap, uint8_t *packet)
{
    struct Writer writer;
    writer_init(&writer, KEYMAP_PACKET_LENGTH, packet);

    writer_u16(&writer, 0x014F);
    writer_u8(&writer, 0);

    for (uint8_t i = 0; i < KEYMAP_MAX_KEYS; i++) {
        writer_u8(&writer, keymap[i].type);
        writer_u32(&writer, keymap[i].action);
    }
}

static void exp_gain_packet_internal(struct Writer *writer, int32_t exp, int32_t equip_bonus, int32_t party_bonus, bool white, bool in_chat)
{
    writer_u16(writer, 0x0027);
    writer_u8(writer, 3);
    writer_bool(writer, white);
    writer_i32(writer, exp);
    writer_bool(writer, in_chat);
    writer_i32(writer, 0); // Bonus event exp
    writer_u8(writer, 0);
    writer_u8(writer, 0);
    writer_i32(writer, 0); // Wedding bonus
    if (in_chat)
        writer_u8(writer, 0);
    writer_u8(writer, 0);
    writer_i32(writer, party_bonus);
    writer_i32(writer, equip_bonus);
    writer_i32(writer, 0); // Internet cafe bonus
    writer_i32(writer, 0); // Rainbow week bonus
}

static void add_skills(void *data, void *ctx)
{
    struct Skill *skill = data;
    struct Writer *writer = ctx;

    writer_u32(writer, skill->id);
    writer_u32(writer, skill->level);
    writer_u64(writer, 150842304000000000L); // DEFAULT_TIME
    // writer_u32(writer, skill->masterLevel); // Only when the skill is a 4th job skill
}

static void add_quests(void *data, void *ctx_)
{
    struct Quest *quest = data;
    struct Writer *writer = ctx_;
    writer_u16(writer, quest->id);
    char str[15];
    quest_get_progress_string(quest, str);
    writer_sized_string(writer, 3 * quest->progressCount, str);
}

static void add_quest_infos(void *data, void *ctx_)
{
    struct QuestInfoProgress *info = data;
    struct Writer *writer = ctx_;
    writer_u16(writer, info->id);
    writer_sized_string(writer, info->length, info->value);
}

static void add_completed_quests(void *data, void *ctx_)
{
    struct CompletedQuest *quest = data;
    struct Writer *writer = ctx_;
    writer_u16(writer, quest->id);
    struct tm tm;
    localtime_r(&quest->time, &tm);
    writer_u64(writer, quest->time * 1000L * 10000L + 116444736010800000L + tm.tm_gmtoff * 1000L * 10000L); // Current time
}

static void add_monster_book_entries(void *data, void *ctx)
{
    struct MonsterBookEntry *entry = data;
    struct Writer *writer = ctx;

    writer_u16(writer, entry->id % 10000);
    writer_i8(writer, entry->count);
}


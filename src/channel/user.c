#include "user.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "scripting/script-manager.h"
#include "scripting/user.h"
#include "../packet.h"
#include "shop.h"

struct User {
    struct Session *session;
    struct Client *client;
    void *script;
    struct {
        struct ScriptManager *quest;
        struct ScriptManager *npc;
        struct ScriptManager *portal;
        struct ScriptManager *map;
    } managers;
};

static bool do_use_item(struct User *user, const struct ConsumableInfo *info, uint8_t *effect);

struct User *user_create(struct Session *session, struct Character *chr, struct ScriptManager *quest_manager, struct ScriptManager *portal_mananger, struct ScriptManager *npc_manager, struct ScriptManager *map_manager)
{
    struct User *user = malloc(sizeof(struct User));
    if (user == NULL)
        return NULL;

    user->client = client_create(chr);
    if (user->client == NULL) {
        free(user);
        return NULL;
    }

    user->session = session;
    user->script = NULL;
    user->managers.quest = quest_manager;
    user->managers.portal = portal_mananger;
    user->managers.npc = npc_manager;
    user->managers.map = map_manager;

    return user;
}

void user_destroy(struct User *user)
{
    if (user != NULL) {
        client_destroy(user->client);
    }

    free(user);
}

int user_portal(struct User *user, uint32_t target, size_t len, char *portal, uint32_t *out_map, uint8_t *out_portal)
{
    struct Session *session = user->session;
    struct Client *client = user->client;

    if (target == (uint32_t)-1) {
        portal[len] = '\0';
        uint32_t target_map = wz_get_target_map(client_map(client), portal);
        if (target_map == (uint32_t)-1) {
            session_shutdown(session); // BAN
            return -1;
        }
        uint8_t target_portal = wz_get_target_portal(client_map(client), portal);
        if (target_portal == (uint8_t)-1) {
            session_shutdown(session); // BAN
            return -1;
        }

        *out_map = target_map;
        *out_portal = target_portal;
    } else {
        if (client_hp(client) > 0) {
            // Special portals that are used at the end of cutscenes
            if (client_map(client) == 0 || client_map(client) / 1000 == 1020) {
                uint32_t id = wz_get_map_forced_return(client_map(client));
                if (id != target) {
                    session_shutdown(session);
                    return -1;
                }

                const struct PortalInfo *portal = wz_get_portal_info_by_name(id, "sp");

                *out_map = target;
                *out_portal = portal->id;
            } else {
                session_shutdown(session); // BAN?
                return -1;
            }
        } else {
            uint32_t id = wz_get_map_nearest_town(client_map(client));
            if (id != target) {
                session_shutdown(session); // BAN
                return -1;
            }

            const struct PortalInfo *portal = wz_get_portal_info_by_name(id, "sp");

            // TODO: Deduct EXP
            client_set_hp(client, 50);
            // The CHANGE_MAP_PACKET will notify the client of its new HP
            /*{
                uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                size_t len = stat_change_packet(true, STAT_HP, &(union StatValue) { .u16 = client_hp(client) }, packet);
                if (session_write(session, len, packet) == -1)
                    return -1;
            }*/

            *out_map = target;
            *out_portal = portal->id;
        }
    }

    return 0;
}

int user_change_map(struct User *user, uint32_t target, uint8_t targetPortal)
{
    struct Session *session = user->session;
    struct Client *client = user->client;
    client_set_map(client, target);
    uint8_t packet[CHANGE_MAP_PACKET_LENGTH];
    change_map_packet(target, targetPortal, client_hp(client), packet);
    return session_write(session, CHANGE_MAP_PACKET_LENGTH, packet);
}

void user_new_map(struct User *user)
{
    struct Session *session = user->session;
    struct Client *client = user->client;

    client_set_map(client, client_get_character(client)->map);

    {
        uint8_t packet[SET_FIELD_PACKET_MAX_LENGTH];
        size_t len = set_field_packet(client_get_character(client), packet);
        if (session_write(session, len, packet) == -1)
            return;
    }

    {
        uint8_t packet[KEYMAP_PACKET_LENGTH];
        keymap_packet(client_get_character(client)->keyMap, packet);
        if (session_write(session, KEYMAP_PACKET_LENGTH, packet) == -1)
            return;
    }

    {
        uint8_t packet[3] = { 0x9F, 0x00, 0x00 }; // Quickslot init
        if (session_write(session, 3, packet) == -1)
            return;
    }

    {
        uint8_t packet[3] = { 0x7C, 0x00, 0x00 }; // Macro init
        if (session_write(session, 3, packet) == -1)
            return;
    }

    {
        uint8_t packet[6] = { 0x50, 0x01, 0x00, 0x00, 0x00, 0x00 }; // Auto HP
        if (session_write(session, 6, packet) == -1)
            return;
    }

    {
        uint8_t packet[6] = { 0x51, 0x01, 0x00, 0x00, 0x00, 0x00 }; // Auto MP
        if (session_write(session, 6, packet) == -1)
            return;
    }

    {
        uint8_t packet[4] = { 0x3F, 0x00, 0x07, 0x00 }; // Buddylist
        if (session_write(session, 4, packet) == -1)
            return;
    }

    {
        uint8_t packet[SET_GENDER_PACKET_LENGTH];
        set_gender_packet(client_get_character(client)->gender, packet);
        if (session_write(session, SET_GENDER_PACKET_LENGTH, packet))
            return;
    }

    {
        uint8_t packet[3] = { 0x2F, 0x00, 0x01 }; // Claim status changed?
        session_write(session, 3, packet);
    }
}

uint32_t user_get_active_npc(struct User *user);
uint16_t user_get_active_quest(struct User *user);
void user_set_hp(struct User *user, int16_t hp)
{
    struct Session *session = user->session;
    struct Client *client = user->client;
    client_set_hp(client, hp);
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    size_t len = stat_change_packet(true, STAT_HP, &(union StatValue) { .u16 = client_hp(client) }, packet);
    session_write(session, len, packet);
}

void user_adjust_hp(struct User *user, int32_t hp)
{
    struct Session *session = user->session;
    struct Client *client = user->client;
    client_adjust_hp(client, true, hp);
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    size_t len = stat_change_packet(true, STAT_HP, &(union StatValue) { .u16 = client_hp(client) }, packet);
    session_write(session, len, packet);
}

void user_set_mp(struct User *user, int16_t mp);
void user_adjust_mp(struct User *user, int16_t mp)
{
    struct Session *session = user->session;
    struct Client *client = user->client;
    client_adjust_mp(client, mp);
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    size_t len = stat_change_packet(true, STAT_MP, &(union StatValue) { .u16 = client_mp(client) }, packet);
    session_write(session, len, packet);
}

int16_t user_mp(struct User *user);
int32_t user_meso(struct User *user)
{
    return client_get_character(user->client)->mesos;
}

void user_adjust_sp(struct User *user, int16_t sp);
bool user_assign_stat(struct User *user, uint32_t stat)
{
    struct Session *session = user->session;
    struct Client *client = user->client;
    uint32_t stats = STAT_AP;
    switch (stat) {
        case 0x40:
            if (!client_adjust_str(client, 1))
                return false;
            stats |= STAT_STR;
        break;
        case 0x80:
            if (!client_adjust_dex(client, 1))
                return false;
            stats |= STAT_DEX;
        break;
        case 0x100:
            if (!client_adjust_int(client, 1))
                return false;
            stats |= STAT_INT;
        break;
        case 0x200:
            if (!client_adjust_luk(client, 1))
                return false;
            stats |= STAT_LUK;
        break;
        default:
            session_shutdown(session);
            return false;
    }

    return user_commit_stats(user, stats);
}

void user_assign_sp(struct User *user, uint32_t id)
{
    struct Session *session = user->session;
    struct Client *client = user->client;

    if (id >= 1000 && id <= 1002) {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, 0, NULL, packet);
        session_write(session, len, packet);
    }

    int8_t level;
    int8_t master;
    if (!client_assign_sp(client, id, &level, &master)) {
        session_shutdown(session); // BAN
        return;
    }

    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, STAT_SP, &(union StatValue) { .i16 = client_sp(client) }, packet);
        session_write(session, len, packet);
    }

    if (level != 0) {
        uint8_t packet[UPDATE_SKILL_PACKET_LENGTH];
        update_skill_packet(id, level, master, packet);
        session_write(session, UPDATE_SKILL_PACKET_LENGTH, packet);
    }
}

void user_change_job(struct User *user, enum Job job);
bool user_gain_exp(struct User *user, int32_t exp, bool reward, bool *leveled)
{
    struct Session *session = user->session;
    struct Client *client = user->client;
    bool leveled_;
    if (leveled == NULL)
        leveled = &leveled_;

    if (!reward) {
        uint8_t packet[EXP_GAIN_PACKET_LENGTH];
        exp_gain_packet(exp, 0, 0, true, packet);
        if (session_write(session, EXP_GAIN_PACKET_LENGTH, packet) == -1)
            return false;
    } else {
        uint8_t packet[EXP_GAIN_IN_CHAT_PACKET_LENGTH];
        exp_gain_in_chat_packet(exp, 0, 0, true, packet);
        if (session_write(session, EXP_GAIN_IN_CHAT_PACKET_LENGTH, packet) == -1)
            return false;
    }

    uint8_t level = client_level(client);
    enum Stat stats;
    uint8_t levels = client_gain_exp(client, exp, &stats);
    // Spam the "level up" effect when multiple level ups occur at once for a nice and satisfying sound
    for (uint8_t i = 0; i < levels - 1; i++) {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        union StatValue value = {
            .u8 = level + i + 1
        };
        size_t size = stat_change_packet(true, STAT_LEVEL, &value, packet);
        if (session_write(session, size, packet) == -1)
            return false;
    }

    user_commit_stats(user, stats);

    *leveled = levels > 0;
    return true;
}

bool user_gain_meso(struct User *user, int32_t mesos, bool pickup, bool reward)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    if (!client_adjust_mesos(client, false, mesos))
        return false;

    if (pickup) {
        uint8_t packet[MESO_GAIN_PACKET_LENGTH];
        meso_gain_packet(mesos, packet);
        if (session_write(session, MESO_GAIN_PACKET_LENGTH, packet) == -1)
            return false;
    } else if (reward) {
        uint8_t packet[MESO_GAIN_IN_CHAT_PACKET_LENGTH];
        meso_gain_in_chat_packet(mesos, packet);
        if (session_write(session, MESO_GAIN_IN_CHAT_PACKET_LENGTH, packet) == -1)
            return false;
    }

    return user_commit_stats(user, STAT_MESO);
}

void user_adjust_fame(struct User *user, int16_t fame);
bool user_commit_stats(struct User *user, enum Stat stats)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    uint8_t value_count = 0;
    union StatValue value[20];
    if (stats & STAT_SKIN) {
        value[value_count].u8 = client_skin(client);
        value_count++;
    }

    if (stats & STAT_FACE) {
        value[value_count].u32 = client_face(client);
        value_count++;
    }

    if (stats & STAT_HAIR) {
        value[value_count].u32 = client_hair(client);
        value_count++;
    }

    if (stats & STAT_LEVEL) {
        value[value_count].u8 = client_level(client);
        value_count++;
    }

    if (stats & STAT_JOB) {
        value[value_count].u16 = client_job(client);
        value_count++;
    }

    if (stats & STAT_STR) {
        value[value_count].i16 = client_str(client);
        value_count++;
    }

    if (stats & STAT_DEX) {
        value[value_count].i16 = client_dex(client);
        value_count++;
    }

    if (stats & STAT_INT) {
        value[value_count].i16 = client_int(client);
        value_count++;
    }

    if (stats & STAT_LUK) {
        value[value_count].i16 = client_luk(client);
        value_count++;
    }

    if (stats & STAT_HP) {
        value[value_count].i16 = client_hp(client);
        value_count++;
    }

    if (stats & STAT_MAX_HP) {
        value[value_count].i16 = client_max_hp(client);
        value_count++;
    }

    if (stats & STAT_MP) {
        value[value_count].i16 = client_mp(client);
        value_count++;
    }

    if (stats & STAT_MAX_MP) {
        value[value_count].i16 = client_max_mp(client);
        value_count++;
    }

    if (stats & STAT_AP) {
        value[value_count].i16 = client_ap(client);
        value_count++;
    }

    if (stats & STAT_SP) {
        value[value_count].i16 = client_sp(client);
        value_count++;
    }

    if (stats & STAT_EXP) {
        value[value_count].i32 = client_exp(client);
        value_count++;
    }

    if (stats & STAT_FAME) {
        value[value_count].i16 = client_fame(client);
        value_count++;
    }

    if (stats & STAT_MESO) {
        value[value_count].i32 = client_meso(client);
        value_count++;
    }

    if (stats & STAT_PET) {
        //value[value_count].u8 = chr->pe;
        //value_count++;
    }

    if (stats & STAT_GACHA_EXP) {
        //value[value_count].i32 = client_gacha_exp(client);
        //value_count++;
    }

    size_t len = stat_change_packet(true, stats, value, packet);
    return session_write(session, len, packet) != -1;
}

bool user_has_item(struct User *user, uint32_t id, int16_t qty)
{
    struct Client *client = user->client;
    return client_has_item(client, id, qty);
}

bool user_gain_items(struct User *user, size_t len, const uint32_t *ids, const int16_t *counts, bool reward, bool *success)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    size_t change_count;
    struct InventoryModify *changes;
    *success = false;
    if (!client_gain_items(client, len, ids, counts, &change_count, &changes)) {
        session_shutdown(session);
        return false;
    }

    if (change_count == 0)
        return true;

    *success = true;

    /*while (change_count > 255) {
        struct InventoryModify mods[255];
        for (size_t i = 0; i < 255; i++)
            change_to_mod(&changes[i], &mods[i]);

        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(255, changes + , packet);
        if (session_write(session, len, packet) == -1)
            return false;
        change_count -= 255;
    }*/

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(change_count, changes, packet);
        if (session_write(session, len, packet) == -1)
            return false;
    }

    if (reward) {
        for (size_t i = 0; i < len; i++) {
            uint8_t packet[ITEM_GAIN_IN_CHAT_PACKET_LENGTH];
            item_gain_in_chat_packet(ids[i], counts[i], packet);
            if (session_write(session, ITEM_GAIN_IN_CHAT_PACKET_LENGTH, packet) == -1)
                return false;
        }
    }

    return true;
}

bool user_gain_inventory_item(struct User *user, const struct InventoryItem *item, uint8_t *effect)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    const struct ItemInfo *info = wz_get_item_info(item->item.itemId);
    if (info->quest) {
        if (client_remaining_quest_item_quantity(client, item->item.itemId) == 0) {
            {
                uint8_t packet[ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH];
                item_unavailable_notification_packet(packet);
                if (session_write(session, ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH, packet) == -1)
                    return false;
            }
            {
                uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                size_t len = stat_change_packet(true, 0, NULL, packet);
                session_write(session, len, packet);
            }
            return false;
        }
    }

    const struct ConsumableInfo *cons_info = wz_get_consumable_info(item->item.itemId);
    if (cons_info != NULL && cons_info->consumeOnPickup) {
        return do_use_item(user, cons_info, effect);
    }

    size_t count;
    struct InventoryModify *changes;
    if (!client_gain_inventory_item(client, item, &count, &changes))
        return false;

    if (info->quest) {
        client_decrease_quest_item(client, info->id, item->quantity);
    }

    struct InventoryModify *changes_temp = changes;
    /*while (count > 255) {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(255, mods, packet);
        session_write(session, len, packet);
        count -= 255;
        changes += 255;
    }*/

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(count, changes, packet);
        session_write(session, len, packet);
    }

    free(changes_temp);

    return true;
}

bool user_gain_equipment(struct User *user, const struct Equipment *item)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    const struct ItemInfo *info = wz_get_item_info(item->item.itemId);
    if (info->quest) {
        if (client_remaining_quest_item_quantity(client, item->item.itemId) == 0) {
            {
                uint8_t packet[ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH];
                item_unavailable_notification_packet(packet);
                if (session_write(session, ITEM_UNAVAILABLE_NOTIFICATION_PACKET_LENGTH, packet) == -1)
                    return false;
            }
            {
                uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                size_t len = stat_change_packet(true, 0, NULL, packet);
                session_write(session, len, packet);
            }
            return false;
        }
    }

    struct InventoryModify change;
    if (!client_gain_equipment(client, item, &change))
        return false;

    if (info->quest)
        client_decrease_quest_item(client, info->id, 1);

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &change, packet);
        session_write(session, len, packet);
    }

    return true;
}

bool user_remove_item(struct User *user, uint8_t inv, uint8_t src, int16_t amount, struct InventoryItem *item)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (inv < 2 || inv > 5) {
        session_shutdown(session);
        return false;
    }

    inv -= 2;
    src--;
    if (src >= client_inventory_slot_count(client, inv)) {
        session_shutdown(session);
        return false;
    }

    struct InventoryModify mod;
    if (!client_remove_item(client, inv, src, amount, &mod, item))
        return false;

    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
    size_t len = modify_items_packet(1, &mod, packet);
    if (session_write(session, len, packet) == -1)
        return false;

    return true;
}

bool user_use_projectile(struct User *user, int16_t amount, uint32_t *id)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    struct InventoryModify mod;
    if (!client_use_projectile(client, amount, id, &mod))
        return false;

    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
    size_t len = modify_items_packet(1, &mod, packet);
    return session_write(session, len, packet) != -1;
}

bool user_remove_equip(struct User *user, bool equipped, uint8_t src, struct Equipment *equip)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    struct InventoryModify mod;
    if (equipped) {
        src = equip_slot_to_compact(src);
        if (src >= EQUIP_SLOT_COUNT) {
            session_shutdown(session);
            return false;
        }

        if (!client_remove_equip(client, true, src, &mod, equip))
            return false;
    } else {
        src--;
        if (src >= client_equip_slot_count(client)) {
            session_shutdown(session);
            return false;
        }

        if (!client_remove_equip(client, false, src, &mod, equip))
            return false;
    }

    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
    size_t len = modify_items_packet(1, &mod, packet);
    return session_write(session, len, packet);
}

bool user_move_item(struct User *user, uint8_t inv, uint8_t src, uint8_t dst)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    uint8_t mod_count;
    struct InventoryModify mods[2];
    if (inv != 1) {
        if (inv > 5) {
            session_shutdown(session);
            return false;
        }

        uint8_t slots = client_inventory_slot_count(client, inv - 2);
        if (src > slots || dst > slots) {
            session_shutdown(session);
            return false;
        }

        mod_count = client_move_item(client, inv, src - 1, dst - 1, mods);
    } else {
        if (dst > 127) {
            mod_count = client_equip(client, src - 1, 256-dst, mods);
        } else {
            mod_count = client_unequip(client, 256-src, dst, mods);
        }
    }

    if (mod_count != 0) {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(mod_count, mods, packet);
        return session_write(session, len, packet) == -1;
    }

    return true;
}

bool user_equip(struct User *user, uint8_t src, enum EquipSlot dst);
bool user_unequip(struct User *user, enum EquipSlot src, uint8_t dst);
bool user_use_item(struct User *user, uint8_t slot, uint32_t id)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (slot > client_equip_slot_count(client)) {
        session_shutdown(session);
        return false;
    }

    const struct ConsumableInfo *info = wz_get_consumable_info(id);
    if (info == NULL) {
        session_shutdown(session);
        return false;
    }

    struct InventoryModify mod;
    if (!client_remove_item(client, 0, slot - 1, 1, &mod, NULL))
        return false;

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        if (session_write(session, len, packet) == -1)
            return false;
    }

    return do_use_item(user, info, NULL);
}

bool user_use_item_immediate(struct User *user, uint32_t id)
{
    return true;
}

bool user_is_quest_started(struct User *user, uint16_t qid);
bool user_is_quest_complete(struct User *user, uint16_t qid);

void user_talk_npc(struct User *user, uint32_t npc)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (user->script != NULL)
        return;

    char script[12];
    snprintf(script, 12, "%d.lua", npc);
    user->script = script_manager_alloc(user->managers.npc, script, 0);
    if (user->script == NULL) {
        session_shutdown(session);
        return;
    }

    client_set_npc(client, npc);

    enum ScriptResult res = script_manager_run(user->script, SCRIPT_USER_TYPE, user);
    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(user->script);
        user->script = NULL;
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(user->script);
        user->script = NULL;
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(user->script);
        user->script = NULL;
    case SCRIPT_RESULT_VALUE_NEXT:
    break;
    }
}

void user_portal_script(struct User *user, const char *portal)
{
    struct Session *session = user->session;

    if (user->script != NULL)
        return;

    char script[21];
    snprintf(script, 21, "%s.lua", portal);
    user->script = script_manager_alloc(user->managers.portal, script, 0);
    if (user->script == NULL) {
        session_shutdown(session);
        return;
    }
    enum ScriptResult res = script_manager_run(user->script, SCRIPT_USER_TYPE, user);
    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
    case SCRIPT_RESULT_VALUE_FAILURE:
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(user->script);
        user->script = NULL;
        if (res != SCRIPT_RESULT_VALUE_SUCCESS)
            session_shutdown(session);
        break;
    case SCRIPT_RESULT_VALUE_NEXT:
        break;
    }
}

struct Character *user_get_character(struct User *user)
{
    struct Client *client = user->client;
    return client_get_character(client);
}

static bool start_quest(struct User *user, const struct QuestInfo *info, uint32_t npc, bool *success);

ssize_t user_start_quest(struct User *user, uint16_t qid, uint32_t npc, bool scripted, uint32_t **item_ids)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (scripted && user->script != NULL)
        return -1;

    const struct QuestInfo *info = wz_get_quest_info(qid);
    if (info == NULL) {
        session_shutdown(session); // BAN
        return -1;
    }

    // TODO: Also check if this quest isn't repeatable
    if (client_is_quest_started(client, qid) || client_is_quest_complete(client, qid)) {
        return -1;
    }

    if ((scripted && !info->startScript) || (!scripted && info->startScript)) {
        session_shutdown(session); // BAN
        return -1;
    }

    // TODO: Some checks should not be possible for honest clients to fail (i.e. NPC check)
    // and should be banned if they fail them
    if (!client_check_start_quest_requirements(client, info, npc))
        return -1;

    if (info->startScript) {
        client_set_npc(client, npc);
        client_set_quest(client, qid);
        char script[10];
        snprintf(script, 10, "%d.lua", qid);
        user->script = script_manager_alloc(user->managers.quest, script, 0);
        enum ScriptResult res = script_manager_run(user->script, SCRIPT_USER_TYPE, user);
        switch (res) {
        case SCRIPT_RESULT_VALUE_KICK:
        case SCRIPT_RESULT_VALUE_FAILURE:
        case SCRIPT_RESULT_VALUE_SUCCESS:
            script_manager_free(user->script);
            user->script = NULL;
            if (res != SCRIPT_RESULT_VALUE_SUCCESS)
                session_shutdown(session);
            return -1;
        case SCRIPT_RESULT_VALUE_NEXT:
            break;
        }

        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_ITEM) {
            const struct ItemInfo *item_info = wz_get_item_info(info->endRequirements[i].item.id);
            if (item_info->quest)
                count++;
        }
    }

    *item_ids = NULL;
    if (count != 0) {
        *item_ids = malloc(count * sizeof(uint32_t));
        if (*item_ids == NULL) {
            session_shutdown(session);
            return -1;
        }

        count = 0;
        for (size_t i = 0; i < info->endRequirementCount; i++) {
            if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_ITEM) {
                const struct ItemInfo *item_info = wz_get_item_info(info->endRequirements[i].item.id);
                if (item_info->quest) {
                    (*item_ids)[count] = item_info->id;
                    count++;
                }
            }
        }
    }

    bool success;
    if (!start_quest(user, info, npc, &success) || !success) {
        free(*item_ids);
        return -1;
    }

    return count;
}

static bool start_quest(struct User *user, const struct QuestInfo *info, uint32_t npc, bool *success)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    uint32_t ids[5];
    size_t progress_count = 0;
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        struct QuestRequirement *req = &info->endRequirements[i];
        if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
            progress_count += req->mob.count;
            for (size_t i = 0; i < req->mob.count; i++) {
                ids[progress_count + i] = req->mob.mobs[i].id;
            }
        }
    }

    if (!client_add_quest(client, info->id, progress_count, ids, success)) {
        session_shutdown(session);
        return false;
    }

    if (!*success)
        return false;

    *success = false;

    uint32_t stats = 0;
    size_t count;
    for (size_t i = 0; i < info->startActCount; i++) {
        switch (info->startActs[i].type) {
        case QUEST_ACT_TYPE_EXP: {
            client_gain_exp(client, info->startActs[i].exp.amount, &stats);
        }
        break;
        case QUEST_ACT_TYPE_MESO:
            client_adjust_mesos(client, true, info->startActs[i].meso.amount);
            stats |= 0x40000;
        break;
        case QUEST_ACT_TYPE_ITEM: {
            bool has_prop = false;
            int8_t item_count = 0;
            int32_t total = 0;
            for (size_t j = 0; j < info->startActs[i].item.count; j++) {
                if (info->startActs[i].item.items[j].prop == 0 || !has_prop) {
                    if (info->startActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    item_count++;
                }

                total += info->startActs[i].item.items[j].prop;
            }

            uint32_t ids[item_count];
            int16_t amounts[item_count];

            int32_t r;
            if (has_prop) {
                // If there is a random item, there should be at least one empty slot in each inventory
                if (!client_has_empty_slot_in_each_inventory(client)) {
                    uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
                    char *message = "Please check if you have enough space in your inventory.";
                    size_t len = popup_message_packet(strlen(message), message, packet);
                    session_write(session, len, packet);
                    return true;
                }

                r = rand() % total;
            }

            has_prop = false;
            item_count = 0;
            total = 0;
            for (size_t j = 0; j < info->startActs[i].item.count; j++) {
                total += info->startActs[i].item.items[j].prop;
                if (info->startActs[i].item.items[j].prop == 0 || (!has_prop && r < total)) {
                    if (info->startActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    ids[item_count] = info->startActs[i].item.items[j].id;
                    amounts[item_count] = info->startActs[i].item.items[j].count;
                    item_count++;
                }
            }

            for (int8_t i = 0; i < item_count; i++) {
                if (client_has_item(client, ids[i], 1)) {
                    item_count--;
                    ids[i] = ids[item_count];
                    amounts[i] = amounts[item_count];
                    i--;
                }
            }

            struct InventoryModify *changes;
            if (!client_gain_items(client, item_count, ids, amounts, &count, &changes)) {
                client_remove_quest(client, info->id);
                session_shutdown(session);
                return false;
            }

            if (count == 0) {
                uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
                char *message = "Please check if you have enough space in your inventory.";
                size_t len = popup_message_packet(strlen(message), message, packet);
                session_write(session, len, packet);
                return true;
            }
        }
        break;

        default:
            fprintf(stderr, "Unimplemented\n");
        }
    }

    // No need to enable actions if no stat has changed
    if (stats != 0)
        user_commit_stats(user, stats);

    char progress[15] = "000000000000000";
    {
        uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
        size_t len = update_quest_packet(info->id, progress_count * 3, progress, packet);
        session_write(session, len, packet);
    }

    {
        uint8_t packet[START_QUEST_PACKET_LENGTH];
        start_quest_packet(info->id, npc, packet);
        session_write(session, START_QUEST_PACKET_LENGTH, packet);
    }

    *success = true;
    return true;
}

void user_regain_quest_item(struct User *user, uint16_t qid, uint32_t id)
{
}

const char *user_get_quest_info(struct User *user, uint16_t info)
{
    return client_get_quest_info(user->client, info);
}

int user_set_quest_info(struct User *user, uint16_t info, const char *value)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    if (!client_set_quest_info(client, info, value)) {
        session_shutdown(session);
        return -1;
    }

    uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
    size_t len = update_quest_packet(info, strlen(value), value, packet);
    if (session_write(session, len, packet) == -1) {
        session_shutdown(session);
        return -1;
    }

    return 0;
}

bool user_start_quest_now(struct User *user, bool *success)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    const struct QuestInfo *info = wz_get_quest_info(client_get_quest(client));

    uint32_t ids[5];
    size_t progress_count = 0;
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        struct QuestRequirement *req = &info->endRequirements[i];
        if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
            progress_count += req->mob.count;
            for (size_t i = 0; i < req->mob.count; i++) {
                ids[progress_count + i] = req->mob.mobs[i].id;
            }
        }
    }

    if (!client_add_quest(client, client_get_quest(client), 1, ids, success))
        return false;

    if (!*success)
        return true;

    char progress[15] = "000000000000000";
    {
        uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
        size_t len = update_quest_packet(info->id, progress_count * 3, progress, packet);
        session_write(session, len, packet);
    }

    {
        uint8_t packet[START_QUEST_PACKET_LENGTH];
        start_quest_packet(info->id, client_get_npc(client), packet);
        session_write(session, START_QUEST_PACKET_LENGTH, packet);
    }

    return true;
}

static bool end_quest(struct User *user, const struct QuestInfo *info, uint32_t npc);

bool user_end_quest(struct User *user, uint16_t qid, uint32_t npc, bool scripted)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (scripted && user->script != NULL)
        return true;

    const struct QuestInfo *info = wz_get_quest_info(qid);
    if (info == NULL) {
        session_shutdown(session);
        return false;
    }

    if (client_is_quest_complete(client, qid) || !client_is_quest_started(client, qid))
        return true;

    if ((scripted && !info->endScript) || (!scripted && info->endScript)) {
        session_shutdown(session);
        return false;
    }

    if (!client_check_end_quest_requirements(client, info, npc))
        return false;

    if (info->endScript) {
        client_set_npc(client, npc);
        client_set_quest(client, qid);
        char script[10];
        snprintf(script, 10, "%d.lua", qid);
        user->script = script_manager_alloc(user->managers.quest, script, 1);
        enum ScriptResult res = script_manager_run(user->script, SCRIPT_USER_TYPE, user);
        switch (res) {
        case SCRIPT_RESULT_VALUE_KICK:
        case SCRIPT_RESULT_VALUE_FAILURE:
        case SCRIPT_RESULT_VALUE_SUCCESS:
            script_manager_free(user->script);
            user->script = NULL;
            if (res != SCRIPT_RESULT_VALUE_SUCCESS)
                session_shutdown(session);
            return false;
        case SCRIPT_RESULT_VALUE_NEXT:
            return true;
        }
    }

    if (!end_quest(user, info, npc)) {
        session_shutdown(session);
        return false;
    }

    return true;
}

static bool end_quest(struct User *user, const struct QuestInfo *info, uint32_t npc)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    uint32_t stats = 0;
    uint16_t next_quest = 0;
    for (size_t i = 0; i < info->endActCount; i++) {
        struct QuestAct *act = &info->endActs[i];
        switch (act->type) {
        case QUEST_ACT_TYPE_EXP:
            client_gain_exp(client, act->exp.amount, &stats);
        break;

        case QUEST_ACT_TYPE_MESO:
            if (!client_adjust_mesos(client, false, info->startActs[i].meso.amount)) {
                return false;
            }

            stats |= STAT_MESO;
        break;

        case QUEST_ACT_TYPE_ITEM: {
            bool has_prop = false;
            int8_t item_count = 0;
            int32_t total = 0;
            for (size_t j = 0; j < info->startActs[i].item.count; j++) {
                if (info->startActs[i].item.items[j].prop == 0 || !has_prop) {
                    if (info->startActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    item_count++;
                }

                total += info->startActs[i].item.items[j].prop;
            }

            uint32_t ids[item_count];
            int16_t amounts[item_count];

            int32_t r;
            if (has_prop) {
                // If there is a random item, there should be at least one empty slot in each inventory
                if (!client_has_empty_slot_in_each_inventory(client)) {
                    uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
                    char *message = "Please check if you have enough space in your inventory.";
                    size_t len = popup_message_packet(strlen(message), message, packet);
                    session_write(session, len, packet);
                    return false;
                }

                r = rand() % total;
            }

            has_prop = false;
            item_count = 0;
            total = 0;
            for (size_t j = 0; j < info->startActs[i].item.count; j++) {
                total += info->startActs[i].item.items[j].prop;
                if (info->startActs[i].item.items[j].prop == 0 || (!has_prop && r < total)) {
                    if (info->startActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    ids[item_count] = info->startActs[i].item.items[j].id;
                    amounts[item_count] = info->startActs[i].item.items[j].count;
                    item_count++;
                }
            }

            for (int8_t i = 0; i < item_count; i++) {
                if (client_has_item(client, ids[i], 1)) {
                    item_count--;
                    ids[i] = ids[item_count];
                    amounts[i] = amounts[item_count];
                    i--;
                }
            }

            size_t count;
            struct InventoryModify *changes;
            if (!client_gain_items(client, item_count, ids, amounts, &count, &changes)) {
                session_shutdown(session);
                return false;
            }

            if (count == 0) {
                uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
                char *message = "Please check if you have enough space in your inventory.";
                size_t len = popup_message_packet(strlen(message), message, packet);
                session_write(session, len, packet);
                return false;
            }
        }
        break;

        case QUEST_ACT_TYPE_NEXT_QUEST: {
            next_quest = info->endActs[i].nextQuest.qid;
        }
        break;

        case QUEST_ACT_TYPE_SKILL: {
            for (size_t i = 0; i < act->skill.count; i++) {
                for (size_t j = 0; j < act->skill.skills[i].jobCount; j++) {
                    if (client_job(client) == act->skill.skills[i].jobs[j]) {
                        const struct QuestSkillAction *skill = &act->skill.skills[i];
                        client_gain_skill(client, skill->id, skill->level, skill->masterLevel);
                        uint8_t packet[UPDATE_SKILL_PACKET_LENGTH];
                        update_skill_packet(skill->id, skill->level, skill->masterLevel, packet);
                        session_write(session, UPDATE_SKILL_PACKET_LENGTH, packet);
                    }
                }
            }
        }
        break;

        default:
            fprintf(stderr, "Unimplemented\n");
        }
    }

    time_t now = time(NULL);
    if (!client_complete_quest(client, info->id, now)) {
        session_shutdown(session);
        return false;
    }

    // No need to enable actions if no stat has changed
    if (stats != 0)
        user_commit_stats(user, stats);

    {
        uint8_t packet[UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH];
        update_quest_completion_time_packet(info->id, now, packet);
        session_write(session, UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH, packet);
    }

    {
        uint8_t packet[SHOW_EFFECT_PACKET_LENGTH];
        show_effect_packet(0x09, packet);
        session_write(session, SHOW_EFFECT_PACKET_LENGTH, packet);
    }

    {
        uint8_t packet[END_QUEST_PACKET_LENGTH];
        end_quest_packet(info->id, npc, next_quest, packet);
        session_write(session, END_QUEST_PACKET_LENGTH, packet);
    }

    return true;
}

bool user_end_quest_now(struct User *user)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    const struct QuestInfo *info = wz_get_quest_info(client_get_quest(client));
    time_t now = time(NULL);
    if (!client_complete_quest(client, info->id, now))
        return false;

    {
        uint8_t packet[UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH];
        update_quest_completion_time_packet(info->id, now, packet);
        session_write(session, UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH, packet);
    }

    {
        uint8_t packet[SHOW_EFFECT_PACKET_LENGTH];
        show_effect_packet(0x09, packet);
        session_write(session, SHOW_EFFECT_PACKET_LENGTH, packet);
    }

    struct SessionCommand cmd = {
        .type = SESSION_COMMAND_EFFECT,
        .effect.effect_id = 0x09
    };

    session_send_command(session, &cmd);

    {
        uint8_t packet[END_QUEST_PACKET_LENGTH];
        end_quest_packet(info->id, client_get_npc(client), 0, packet);
        session_write(session, END_QUEST_PACKET_LENGTH, packet);
    }

    return true;
}

bool user_forfeit_quest(struct User *user, uint16_t qid)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    if (!client_remove_quest(client, qid))
        return false;

    {
        uint8_t packet[FORFEIT_QUEST_PACKET_LENGTH];
        forfeit_quest_packet(qid, packet);
        return session_write(session, FORFEIT_QUEST_PACKET_LENGTH, packet) != -1;
    }
}

bool user_script_cont(struct User *user, uint8_t prev, uint8_t action, uint32_t selection)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    if (user->script == NULL)
        //session_shutdown(session);
        return true;

    if (!client_is_dialogue_option_legal(client, prev)) {
        script_manager_free(user->script);
        user->script = NULL;
        session_shutdown(session);
        return true;
    }

    // Check if prev matches the actual previous state

    // In an empty dialogue (GET_NUMBER or SIMPLE), END CHAT is (action == 0) instead of (action == -1)
    bool is_empty_dialoge = prev == 3 || prev == 4;
    if ((is_empty_dialoge && action == 0) || (!is_empty_dialoge && action == (uint8_t)-1)) {
        script_manager_free(user->script);
        user->script = NULL;
        return true;
    }

    // In an empty dialogue, the only other option for action is a '1'
    if (is_empty_dialoge && action != 1) {
        script_manager_free(user->script);
        user->script = NULL;
        session_shutdown(session);
        return true;
    }

    uint32_t script_action;
    if (!client_dialogue_is_action_valid(client, action, selection, &script_action)) {
        script_manager_free(user->script);
        user->script = NULL;
        session_shutdown(session); // BAN
        return true;
    }

    enum ScriptResult res = script_manager_run(user->script, script_action);

    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
    case SCRIPT_RESULT_VALUE_FAILURE:
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(user->script);
        user->script = NULL;
        if (res != SCRIPT_RESULT_VALUE_SUCCESS)
            session_shutdown(session);
        break;
    case SCRIPT_RESULT_VALUE_NEXT:
        return false;
    }

    return true;
}

void user_close_script(struct User *user);

static void do_notify_update_progress(uint16_t qid, size_t progress_count, int32_t *progress, void *ctx_);

bool user_kill_monsters(struct User *user, size_t count, uint32_t *ids, bool *leveled)
{
    struct Session *session = user->session;
    struct Client *client = user->client;

    *leveled = false;
    for (size_t i = 0; i < count; i++) {
        struct {
            struct Session *session;
            bool success;
        } ctx = { session, true };
        client_kill_monster(client, ids[i], do_notify_update_progress, &ctx);
        bool did_leveled;
        if (!user_gain_exp(user, wz_get_monster_stats(ids[i])->exp, false, &did_leveled))
            return false;

        if (did_leveled)
            *leveled = true;
    }

    return true;
}

static void do_notify_update_progress(uint16_t qid, size_t progress_count, int32_t *progress, void *ctx_)
{
    struct {
        struct Session *session;
        bool success;
    } *ctx = ctx_;
    if (!ctx->success)
        return;

    char progress_str[15];
    for (size_t i = 0; i < progress_count; i++)
        snprintf(progress_str + 3*i, 4, "%03" PRId32, progress[i]);

    uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
    size_t len = update_quest_packet(qid, progress_count * 3, progress_str, packet);
    ctx->success = ctx->success && session_write(ctx->session, len, packet);
}

bool user_take_damage(struct User *user, int32_t damage)
{
    struct Client *client = user->client;
    client_adjust_hp(client, true, damage);
    user_commit_stats(user, STAT_HP);
    return true;
}

bool user_chair(struct User *user, uint16_t id)
{
    struct Session *session = user->session;
    struct Client *client = user->client;

    if (!ITEM_IS_CHAIR(id)) {
        session_shutdown(session); // BAN
        return false;
    }

    return client_has_item(client, id, 1);
}

void user_destroy_reactor(struct User *user);
void user_open_shop(struct User *user, uint32_t id)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (client_open_shop(client, id)) {
        const struct ShopInfo *info = shop_info_find(id);
        if (info == NULL) {
            session_shutdown(session);
            return;
        }

        struct ShopItem items[info->count];
        for (size_t i = 0; i < info->count; i++) {
            items[i].id = info->items[i].id;
            items[i].price = info->items[i].price;
        }

        uint8_t packet[OPEN_SHOP_PACKET_MAX_LENGTH];
        size_t len = open_shop_packet(id, info->count, items, packet);
        if (session_write(session, len, packet) == -1) {
            session_shutdown(session);
            return;
        }
    }
}

void user_buy(struct User *user, uint16_t pos, uint32_t id, int16_t quantity, int32_t price)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    uint32_t shop_id = client_shop(client);
    if (shop_id == (uint32_t)-1) {
        session_shutdown(session);
        return;
    }

    const struct ShopInfo *shop = shop_info_find(shop_id);
    if (pos >= shop->count) {
        session_shutdown(session);
        return;
    }

    // Also implicitly checks that the item ID actually exists
    if (shop->items[pos].id != id) {
        session_shutdown(session);
        return;
    }

    if (quantity <= 0 || quantity > 1000) {
        session_shutdown(session);
        return;
    }

    // Can't buy more than 1 equipment at a time
    if (ITEM_IS_EQUIP(shop->items[pos].id) && quantity > 1) {
        session_shutdown(session);
        return;
    }

    // TODO: Check if price is per item or total
    if (shop->items[pos].price != price) {
        session_shutdown(session);
        return;
    }

    if (!client_adjust_mesos(client, false, price)) {
        uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
        shop_action_response(2, packet);
        session_write(session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);
        return;
    }

    size_t count;
    struct InventoryModify *mods;
    if (!client_gain_items(client, 1, &id, &quantity, &count, &mods)) {
        client_adjust_mesos(client, false, -price);
        session_shutdown(session);
        return;
    }

    if (count == 0) {
        client_adjust_mesos(client, false, -price);
        uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
        shop_action_response(3, packet);
        session_write(session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);
        return;
    }

    if (!user_commit_stats(user, STAT_MESO))
        return;

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(count, mods, packet);
        free(mods);
        session_write(session, len, packet);
    }
}

void user_sell(struct User *user, uint16_t pos, uint32_t id, int16_t quantity)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (quantity <= 0) {
        session_shutdown(session); // BAN
        return;
    }

    // TODO: Check if item is sellable
    const struct ItemInfo *info = wz_get_item_info(id);
    if (info == NULL) {
        session_shutdown(session); // BAN
        return;
    }

    uint8_t inv = id / 1000000;
    struct InventoryModify change;
    if (inv == 1) {
        if (quantity > 1) {
            session_shutdown(session); // BAN
            return;
        }

        if (pos > client_equip_slot_count(client)) {
            session_shutdown(session); // BAN
            return;
        }

        if (client_get_equip(client, false, pos-1) == (uint32_t)-1)
            return;

        if (client_get_equip(client, false, pos-1) != id) {
            session_shutdown(session); // BAN
            return;
        }

        if (!client_remove_equip(client, false, pos-1, &change, NULL))
            return; // This theoratically shouldn't happen

        client_adjust_mesos(client, true, info->price);
    } else {
        if (pos > client_inventory_slot_count(client, inv-2)) {
            session_shutdown(session); // BAN
            return;
        }

        if (client_get_item(client, inv-2, pos-1) == (uint32_t)-1)
            return;

        if (client_get_item(client, inv-2, pos-1) != id) {
            session_shutdown(session); // BAN
            return;
        }

        if (!client_remove_item(client, inv-2, pos-1, quantity, &change, NULL)) {
            uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
            shop_action_response(0x5, packet);
            session_write(session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);
            return;
        }

        client_adjust_mesos(client, true, info->price * quantity);
    }

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &change, packet);
        if (session_write(session, len, packet) == -1)
            return;
    }
    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, STAT_MESO, &(union StatValue) { .i32 = client_meso(client) }, packet);
        if (session_write(session, len, packet) == -1)
            return;
    }
    {
        uint8_t packet[SHOP_ACTION_RESPONSE_PACKET_LENGTH];
        shop_action_response(8, packet);
        session_write(session, SHOP_ACTION_RESPONSE_PACKET_LENGTH, packet);
    }
}

void user_recharge(struct User *user, uint16_t pos)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
}

bool user_close_shop(struct User *user)
{
    struct Client *client = user->client;

    return client_close_shop(client);
}

void user_send_ok(struct User *user, size_t msg_len, const char *msg, uint8_t speaker)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_OK);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client_get_npc(client), NPC_DIALOGUE_TYPE_OK, msg_len, msg, speaker, packet);
    session_write(session, len, packet);
}

void user_send_yes_no(struct User *user, size_t msg_len, const char *msg, uint8_t speaker)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_YES_NO);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client_get_npc(client), NPC_DIALOGUE_TYPE_YES_NO, msg_len, msg, speaker, packet);
    session_write(session, len, packet);
}

void user_send_simple(struct User *user, size_t msg_len, const char *msg, uint8_t speaker, uint32_t selection_count)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_SIMPLE, selection_count);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client_get_npc(client), NPC_DIALOGUE_TYPE_SIMPLE, msg_len, msg, speaker, packet);
    session_write(session, len, packet);
}

void user_send_next(struct User *user, size_t msg_len, const char *msg, uint8_t speaker)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_NEXT);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client_get_npc(client), NPC_DIALOGUE_TYPE_NEXT, msg_len, msg, speaker, packet);
    session_write(session, len, packet);
}

void user_send_prev_next(struct User *user, size_t msg_len, const char *msg, uint8_t speaker)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_PREV_NEXT);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client_get_npc(client), NPC_DIALOGUE_TYPE_PREV_NEXT, msg_len, msg, speaker, packet);
    session_write(session, len, packet);
}

void user_send_prev(struct User *user, size_t msg_len, const char *msg, uint8_t speaker)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_PREV);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client_get_npc(client), NPC_DIALOGUE_TYPE_PREV, msg_len, msg, speaker, packet);
    session_write(session, len, packet);
}

void user_send_accept_decline(struct User *user, size_t msg_len, const char *msg, uint8_t speaker)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_ACCEPT_DECILNE);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client_get_npc(client), NPC_DIALOGUE_TYPE_ACCEPT_DECILNE, msg_len, msg, speaker, packet);
    session_write(session, len, packet);
}

void user_send_get_number(struct User *user, size_t msg_len, const char *msg, uint8_t speaker, int32_t def, int32_t min, int32_t max)
{
    struct Client *client = user->client;
    struct Session *session = user->session;
    client_set_dialogue_state(client, CLIENT_DIALOGUE_STATE_GET_NUMBER, min, max);
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_get_number_packet(client_get_npc(client), msg_len, msg, speaker, def, min, max, packet);
    session_write(session, len, packet);
}

void user_message(struct User *user, const char *msg)
{
    struct Session *session = user->session;
    uint8_t packet[SERVER_MESSAGE_PACKET_MAX_LENGTH];
    size_t len = server_message_packet(strlen(msg), msg, packet);
    session_write(session, len, packet);
}

void user_reset_stats(struct User *user);
/*void user_launch_portal_script(struct User *user, const char *portal)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (user->script != NULL)
        return;

    char script[12];
    snprintf(script, 12, "%s.lua", portal);
    user->script = script_manager_alloc(user->managers.portal, script, 0);
    if (user->script == NULL) {
        session_shutdown(session);
        return;
    }

    enum ScriptResult res = script_manager_run(user->script, SCRIPT_USER_TYPE, user);
    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(user->script);
        user->script = NULL;
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(user->script);
        user->script = NULL;
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(user->script);
        user->script = NULL;
    case SCRIPT_RESULT_VALUE_NEXT:
    break;
    }

}*/

/*void user_launch_map_script(struct User *user, uint32_t map)
{
    char script[32];
    snprintf(script, 32, "%s.lua", wz_get_map_enter_script(map));
    client->script = script_manager_alloc(client->managers.map, script, 0);

    enum ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
    switch (res) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_BAN };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    case SCRIPT_RESULT_VALUE_NEXT:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_NEXT };
        break;
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

}*/

void user_enable_actions(struct User *user)
{
    user_commit_stats(user, 0);
}

void user_toggle_auto_pickup(struct User *user);
bool user_is_auto_pickup_enabled(struct User *user);
bool user_use_skill(struct User *user, uint32_t skill, int8_t *level, uint32_t *projectile)
{
    struct Client *client = user->client;
    struct Session *session = user->session;

    if (!client_has_skill(client, skill, level)) {
        session_shutdown(session);
        return false;
    }

    const struct SkillInfo *info = wz_get_skill_info(skill);
    if (info == NULL) {
        session_shutdown(session);
        return false; // Packet-edit
    }

    if (!client_adjust_hp_mp(client, -info->levels[*level-1].hpCon, -info->levels[*level-1].mpCon)) {
        return false;
    }

    if (info->levels[*level-1].bulletCount > 0) {
        if (projectile == NULL) {
            // User requested a skill that uses a projectile not through a RANGED_ATTACK
            session_shutdown(session);
            return false; // Packet-edit
        }

        if (!user_use_projectile(user, info->levels[*level-1].bulletCount, projectile)) {
            client_adjust_hp_mp(client, info->levels[*level-1].hpCon, info->levels[*level-1].mpCon);
            return false;
        }
    } else if (projectile != NULL) {
        // User requested a skill that doesn't use a projectile through a RANGED_ATTACK
        session_shutdown(session);
        return false; // Packet-edit
    }

    if (!user_commit_stats(user, STAT_HP | STAT_MP))
        return false;

    return true;
}

bool user_add_key(struct User *user, uint32_t key, uint8_t type, uint32_t action)
{
    struct Client *client = user->client;
    return client_add_key(client, key, type, action);
}

bool user_add_skill_key(struct User *user, uint32_t key, uint32_t skill_id)
{
    struct Client *client = user->client;
    return client_add_skill_key(client, key, skill_id);
}

bool user_remove_key(struct User *user, uint32_t key, uint32_t action)
{
    return false;
}

bool user_sit(struct User *user, uint32_t id);
bool user_sit_on_map_seat(struct User *user, uint16_t id);
bool user_stand_up(struct User *user);
bool user_open_storage(struct User *user);
void user_show_info(struct User *user, const char *path)
{
    struct Session *session = user->session;
    uint8_t packet[SHOW_INFO_PACKET_MAX_LENGTH];
    size_t len = show_info_packet(strlen(path), path, packet);
    session_write(session, len, packet);
}

void user_show_intro(struct User *user, const char *path)
{
    struct Session *session = user->session;
    uint8_t packet[SHOW_INTRO_PACKET_MAX_LENGTH];
    size_t len = show_intro_packet(strlen(path), path, packet);
    session_write(session, len, packet);
}

void user_create_party(struct User *user);
void user_invite_to_party(struct User *user, uint8_t name_len, const char *name);
void user_reject_party_invitaion(struct User *user, uint8_t name_len, const char *name);
void user_announce_party_join(struct User *user, uint32_t id);
void user_announce_party_leave(struct User *user, uint32_t id);
void user_announce_party_kick(struct User *user, uint32_t id);
void user_announce_party_disband(struct User *user);
void user_announce_party_change_online_status(struct User *user, uint32_t id);
void user_announce_party_change_leader(struct User *user, uint32_t id);

static bool do_use_item(struct User *user, const struct ConsumableInfo *info, uint8_t *effect)
{
    uint8_t effect_;
    if (effect == NULL)
        effect = &effect_;

    struct Client *client = user->client;
    struct Session *session = user->session;

    enum Stat stats = 0;
    size_t value_count = 0;
    union StatValue values[2];

    if (info->hp != 0 || info->hpR != 0) {
        client_adjust_hp(client, true, info->hp);
        client_adjust_hp_precent(client, info->hpR);

        values[value_count].i16 = client_hp(client);
        stats |= STAT_HP;
        value_count++;
    }

    if (info->mp != 0 || info->mpR != 0) {
        client_adjust_mp(client, info->mp);
        client_adjust_mp_precent(client, info->mpR);

        values[value_count].i16 = client_mp(client);
        stats |= STAT_MP;
        value_count++;
    }

    if (wz_get_item_info(info->id)->monsterBook) {
        uint8_t count;
        if (!client_record_monster_book_entry(client, info->id, &count))
            return false;

        *effect = 0x0D;
        if (count != 0) {
            {
                uint8_t packet[ADD_CARD_PACKET_LENGTH];
                add_card_packet(false, info->id, count, packet);
                if (session_write(session, ADD_CARD_PACKET_LENGTH, packet) == -1)
                    return false;
            }

            {
                uint8_t packet[SHOW_EFFECT_PACKET_LENGTH];
                show_effect_packet(0x0D, packet);
                if (session_write(session, SHOW_EFFECT_PACKET_LENGTH, packet) == -1)
                    return false;
            }
        } else {
            uint8_t packet[ADD_CARD_PACKET_LENGTH];
            add_card_packet(true, info->id, 5, packet);
            if (session_write(session, ADD_CARD_PACKET_LENGTH, packet) == -1)
                return false;
        }
    }

    // Also enables actions if value_count is 0
    return user_commit_stats(user, stats);
}


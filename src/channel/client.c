#include "client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../constants.h"
#include "../packet.h"
#include "scripting/script-manager.h"
#include "scripting/client.h"
#include "../hash-map.h"

static bool check_quest_requirements(struct Character *chr, size_t req_count, const struct QuestRequirement *reqs, uint32_t npc);
static bool start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success);
static bool end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success);

void client_set_hp(struct Client *client, int16_t hp)
{
    struct Character *chr = &client->character;
    if (hp < 0)
        hp = 0;

    if (hp > character_get_effective_hp(chr))
        hp = character_get_effective_hp(chr);

    client->character.hp = hp;
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value = { .i16 = hp };
    size_t len = stat_change_packet(true, STAT_HP, &value, packet);
    session_write(client->session, len, packet);
}

void client_adjust_hp(struct Client *client, int32_t hp)
{
    if (hp > 0 && client->character.hp > INT16_MAX - hp)
        hp = INT16_MAX - client->character.hp;
    client_set_hp(client, client->character.hp + hp);
}

void client_set_mp(struct Client *client, int16_t mp)
{
    struct Character *chr = &client->character;
    if (mp < 0)
        mp = 0;

    if (mp > character_get_effective_mp(chr))
        mp = character_get_effective_mp(chr);

    client->character.mp = mp;
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value = { .i16 = mp };
    size_t len = stat_change_packet(true, STAT_MP, &value, packet);
    session_write(client->session, len, packet);
}

void client_adjust_mp(struct Client *client, int16_t mp)
{
    if (mp > 0 && client->character.mp > INT16_MAX - mp)
        mp = INT16_MAX - client->character.mp;
    client_set_mp(client, client->character.mp + mp);
}

void client_adjust_sp(struct Client *client, int16_t sp)
{
    struct Character *chr = &client->character;
    if (sp > 0 && chr->sp > INT16_MAX - sp)
        sp = INT16_MAX - chr->sp;

    chr->sp += sp;
    if (chr->sp < 0)
        chr->sp = 0;

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value = { .i16 = chr->sp };
    size_t len = stat_change_packet(true, STAT_SP, &value, packet);
    session_write(client->session, len, packet);
}

void client_raise_str(struct Client *client)
{
    if (client->character.ap > 0) {
        client->character.str++;
        client->character.ap--;
    }

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value[] = {
        {
            .i16 = client->character.str,
        },
        {
            .i16 = client->character.ap,
        }
    };
    size_t len = stat_change_packet(true, STAT_STR | STAT_AP, value, packet);
    session_write(client->session, len, packet);
}

void client_raise_dex(struct Client *client)
{
    if (client->character.ap > 0) {
        client->character.dex++;
        client->character.ap--;
    }

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value[] = {
        {
            .i16 = client->character.dex,
        },
        {
            .i16 = client->character.ap,
        }
    };
    size_t len = stat_change_packet(true, STAT_DEX | STAT_AP, value, packet);
    session_write(client->session, len, packet);
}

void client_raise_int(struct Client *client)
{
    if (client->character.ap > 0) {
        client->character.int_++;
        client->character.ap--;
    }

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value[] = {
        {
            .i16 = client->character.int_,
        },
        {
            .i16 = client->character.ap,
        }
    };
    size_t len = stat_change_packet(true, STAT_INT | STAT_AP, value, packet);
    session_write(client->session, len, packet);
}

void client_raise_luk(struct Client *client)
{
    if (client->character.ap > 0) {
        client->character.luk++;
        client->character.ap--;
    }

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value[] = {
        {
            .i16 = client->character.luk,
        },
        {
            .i16 = client->character.ap,
        }
    };
    size_t len = stat_change_packet(true, STAT_LUK | STAT_AP, value, packet);
    session_write(client->session, len, packet);
}

bool client_assign_sp(struct Client *client, uint32_t id)
{
    struct Character *chr = &client->character;

    // TODO: Check if id is legal for this client to assign

    if (id >= 1000 && id <= 1002) {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, 0, NULL, packet);
        session_write(client->session, len, packet);
    } else if (chr->sp > 0) {
        chr->sp--;
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        union StatValue value = {
            .i16 = chr->sp,
        };
        size_t len = stat_change_packet(true, STAT_SP, &value, packet);
        session_write(client->session, len, packet);
    } else {
        return true;
    }

    struct Skill *skill = hash_set_u32_get(chr->skills, id);
    if (skill == NULL) {
        struct Skill new = {
            .id = id,
            .level = 1,
            .masterLevel = 0
        };
        hash_set_u32_insert(chr->skills, &new);
        skill = hash_set_u32_get(chr->skills, id);
    } else {
        skill->level++;
    }

    {
        uint8_t packet[UPDATE_SKILL_PACKET_LENGTH];
        update_skill_packet(id, skill->level, skill->masterLevel, packet);
        session_write(client->session, UPDATE_SKILL_PACKET_LENGTH, packet);
    }

    return true;
}

void client_gain_exp(struct Client *client, int32_t exp, bool reward)
{
    struct Character *chr = &client->character;
    if (!reward) {
        uint8_t packet[EXP_GAIN_PACKET_LENGTH];
        exp_gain_packet(exp, 0, 0, true, packet);
        session_write(client->session, EXP_GAIN_PACKET_LENGTH, packet);
    } else {
        uint8_t packet[EXP_GAIN_IN_CHAT_PACKET_LENGTH];
        exp_gain_in_chat_packet(exp, 0, 0, true, packet);
        session_write(client->session, EXP_GAIN_IN_CHAT_PACKET_LENGTH, packet);
    }

    // This outer loop should only run 2 times at most
    do {
        if (chr->exp > INT32_MAX - exp) {
            exp -= INT32_MAX - chr->exp;
            chr->exp = INT32_MAX;
        } else {
            chr->exp += exp;
            exp = 0;
        }
        while (chr->exp >= EXP_TABLE[chr->level - 1]) {
            if ((chr->job == JOB_BEGINNER || chr->job == JOB_NOBLESSE || chr->job == JOB_LEGEND) && chr->level <= 10) {
                if (chr->level <= 5) {
                    chr->str += 5;
                } else {
                    chr->str += 4;
                    chr->dex += 1;
                }
            } else {
                if (chr->job != JOB_BEGINNER && chr->job != JOB_NOBLESSE && chr->job != JOB_LEGEND)
                    chr->sp += 3;

                int8_t ap = 5;
                if (job_type(chr->job) == JOB_TYPE_CYGNUS) {
                    if (chr->level > 10) {
                        if (chr->level <= 17)
                            ap += 2;
                        else if (chr->level < 77)
                            ap++;
                    }
                }
                chr->ap += ap;
            }

            if (chr->job == JOB_BEGINNER || chr->job == JOB_NOBLESSE || chr->job == JOB_LEGEND) {
                chr->maxHp += rand() % 5 + 12;
                chr->maxMp += rand() % 3 + 10;
            } else if (job_is_a(chr->job, JOB_FIGHTER) || job_is_a(chr->job, JOB_DAWN_WARRIOR)) {
                chr->maxHp += rand() % 5 + 24;
                chr->maxMp += rand() % 3 + 4;
            } else if (job_is_a(chr->job, JOB_MAGICIAN) || job_is_a(chr->job, JOB_BLAZE_WIZARD)) {
                chr->maxHp += rand() % 5 + 10;
                chr->maxMp += rand() % 3 + 22;
            } else if (job_is_a(chr->job, JOB_ARCHER) || job_is_a(chr->job, JOB_ROGUE) || job_is_a(chr->job, JOB_WIND_ARCHER) || job_is_a(chr->job, JOB_NIGHT_WALKER)) {
                chr->maxHp += rand() % 5 + 20;
                chr->maxMp += rand() % 3 + 14;
            } else if (job_is_a(chr->job, JOB_PIRATE) || job_is_a(chr->job, JOB_THUNDER_BREAKER)) {
                chr->maxHp += rand() % 7 + 22;
                chr->maxMp += rand() % 6 + 18;
            } else if (job_is_a(chr->job, JOB_ARAN)) {
                chr->maxHp += rand() % 5 + 44;
                chr->maxMp += rand() % 5 + 4;
            }

            chr->maxMp += character_get_effective_int(chr) / (job_is_a(chr->job, JOB_MAGICIAN) || job_is_a(chr->job, JOB_BLAZE_WIZARD) ? 20 : 10);

            chr->hp = character_get_effective_hp(chr);
            chr->mp = character_get_effective_mp(chr);

            chr->exp -= EXP_TABLE[chr->level - 1];
            chr->level++;

            {
                uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
                if ((chr->job == JOB_BEGINNER || chr->job == JOB_NOBLESSE || chr->job == JOB_LEGEND) && chr->level <= 10) {
                    union StatValue values[] = {
                        {
                            .u8 = client->character.level,
                        },
                        {
                            .i16 = client->character.str,
                        },
                        {
                            .i16 = client->character.dex,
                        },
                        {
                            .i16 = client->character.hp,
                        },
                        {
                            .i16 = client->character.maxHp,
                        },
                        {
                            .i16 = client->character.mp,
                        },
                        {
                            .i16 = client->character.maxMp,
                        },
                        {
                            .i32 = client->character.exp
                        }
                    };
                    size_t size = stat_change_packet(true, STAT_LEVEL | STAT_STR | STAT_DEX | STAT_HP | STAT_MAX_HP | STAT_MP | STAT_MAX_MP | STAT_EXP, values, packet);
                    session_write(client->session, size, packet);
                } else {
                    union StatValue values[] = {
                        {
                            .u8 = client->character.level,
                        },
                        {
                            .i16 = client->character.hp,
                        },
                        {
                            .i16 = client->character.maxHp,
                        },
                        {
                            .i16 = client->character.mp,
                        },
                        {
                            .i16 = client->character.maxMp,
                        },
                        {
                            .i16 = client->character.ap,
                        },
                        {
                            .i16 = client->character.sp,
                        },
                        {
                            .i32 = client->character.exp
                        }
                    };
                    size_t size = stat_change_packet(true, STAT_LEVEL | STAT_HP | STAT_MAX_HP | STAT_MP | STAT_MAX_MP | STAT_AP | STAT_SP | STAT_EXP, values, packet);
                    session_write(client->session, size, packet);
                }
            }

            {
                uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
                show_foreign_effect_packet(client->character.id, 0, packet);
                session_broadcast_to_room(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
            }
        }
    } while (exp > 0);

    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        union StatValue value = { .i32 = client->character.exp };
        size_t len = stat_change_packet(false, STAT_EXP, &value, packet);
        session_write(client->session, len, packet);
    }
}

void client_gain_meso(struct Client *client, int32_t mesos, bool pickup, bool reward)
{
    if (mesos > 0) {
        if (client->character.mesos > INT32_MAX - mesos)
            mesos = INT32_MAX - client->character.mesos;
    } else {
        if (client->character.mesos + mesos < 0)
            mesos = client->character.mesos;
    }

    client->character.mesos += mesos;
    if (pickup) {
        uint8_t packet[MESO_GAIN_PACKET_LENGTH];
        meso_gain_packet(mesos, packet);
        session_write(client->session, MESO_GAIN_PACKET_LENGTH, packet);
    } else if (reward) {
        uint8_t packet[MESO_GAIN_IN_CHAT_PACKET_LENGTH];
        meso_gain_in_chat_packet(mesos, packet);
        session_write(client->session, MESO_GAIN_IN_CHAT_PACKET_LENGTH, packet);
    }

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value = { .i32 = client->character.mesos };
    size_t len = stat_change_packet(true, STAT_MESO, &value, packet);
    session_write(client->session, len, packet);
}

void client_adjust_fame(struct Client *client, int16_t fame)
{
    if (fame > 0 && client->character.fame > INT16_MAX - fame)
        fame = INT16_MAX - client->character.fame;

    if (fame < 0 && client->character.fame < INT16_MIN - fame)
        fame = INT16_MIN - client->character.fame;

    client->character.fame += fame;
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue value = { .i16 = client->character.fame };
    size_t len = stat_change_packet(true, STAT_FAME, &value, packet);
    session_write(client->session, len, packet);
}

bool client_has_item(struct Client *client, uint32_t id)
{
    uint8_t inv = id / 1000000;
    if (inv == 1) {
        for (uint8_t i = 0; i < client->character.equipmentInventory.slotCount; i++) {
            if (!client->character.equipmentInventory.items[i].isEmpty && client->character.equipmentInventory.items[i].equip.item.itemId == id)
                return true;
        }
    } else {
        inv -= 2;
        for (uint8_t i = 0; i < client->character.inventory[inv].slotCount; i++) {
            if (!client->character.inventory[inv].items[i].isEmpty && client->character.inventory[inv].items[i].item.item.itemId == id)
                return true;
        }
    }

    return false;
}

bool client_gain_items(struct Client *client, size_t len, const uint32_t *ids, const int16_t *counts, bool reward, bool *success)
{
    // Copy the character's inventories, start trying to insert the items, if we successfuly inserted all items copy the inventories back
    struct Character *chr = &client->character;
    struct InventoryModify *mods = malloc(len * sizeof(struct InventoryModify));
    if (mods == NULL)
        return false;

    size_t mod_capacity = len;
    size_t mod_count = 0;

    int16_t amounts[len];
    for (size_t i = 0; i < len; i++)
        amounts[i] = counts[i];

    struct Inventory invs[4];
    struct {
        uint8_t slotCount;
        struct {
            bool isEmpty;
            struct Equipment equip;
        } items[MAX_ITEM_COUNT];
    } equip_inv;

    for (uint8_t i = 0; i < 4; i++)
        invs[i] = chr->inventory[i];

    equip_inv.slotCount = chr->equipmentInventory.slotCount;
    for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        equip_inv.items[i].isEmpty = chr->equipmentInventory.items[i].isEmpty;
        equip_inv.items[i].equip = chr->equipmentInventory.items[i].equip;
    }

    *success = false;
    // Try to remove items before adding other ones
    for (size_t i = 0; i < len; i++) {
        if (amounts[i] < 0) {
            uint8_t inv = ids[i] / 1000000;
            if (inv == 1) {
                for (size_t j = 0; j < equip_inv.slotCount; j++) {
                    if (!equip_inv.items[j].isEmpty && equip_inv.items[j].equip.item.itemId == ids[i]) {
                        equip_inv.items[j].isEmpty = true;
                        amounts[i]++;
                        if (mod_count == mod_capacity) {
                            mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                            mod_capacity *= 2;
                        }

                        mods[mod_count].mode = INVENTORY_MODIFY_TYPE_REMOVE;
                        mods[mod_count].inventory = 1;
                        mods[mod_count].slot = j + 1;
                        mod_count++;
                        if (amounts[i] == 0)
                            break;
                    }
                }

                if (amounts[i] < 0)
                    return true;
            } else {
                inv -= 2;
                for (size_t j = 0; j < invs[inv].slotCount; j++) {
                    if (!invs[inv].items[j].isEmpty && invs[inv].items[j].item.item.itemId == ids[i]) {
                        if (invs[inv].items[j].item.quantity + amounts[i] > 0) {
                            invs[inv].items[j].item.quantity += amounts[i];
                            amounts[i] = 0;
                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mods[mod_count].quantity = invs[inv].items[j].item.quantity;
                            mod_count++;
                        } else {
                            amounts[i] += invs[inv].items[j].item.quantity;
                            invs[inv].items[j].isEmpty = true;
                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_REMOVE;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mod_count++;
                        }
                    }

                    if (amounts[i] == 0)
                        break;
                }
            }
        }
    }

    for (size_t i = 0; i < len; i++) {
        if (amounts[i] > 0) {
            uint8_t inv = ids[i] / 1000000;
            if (inv == 1) {
                for (size_t j = 0; j < equip_inv.slotCount; j++) {
                    if (equip_inv.items[j].isEmpty) {
                        equip_inv.items[j].isEmpty = false;
                        equip_inv.items[j].equip = equipment_from_info(wz_get_equip_info(ids[i]));

                        amounts[i]--;
                        if (mod_count == mod_capacity) {
                            mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                            mod_capacity *= 2;
                        }

                        mods[mod_count].mode = INVENTORY_MODIFY_TYPE_ADD;
                        mods[mod_count].inventory = 1;
                        mods[mod_count].slot = j + 1;
                        mods[mod_count].equip = equip_inv.items[j].equip;
                        mod_count++;
                        if (amounts[i] == 0)
                            break;
                    }
                }

                if (amounts[i] > 0) {
                    free(mods);
                    return true;
                }
            } else {
                inv -= 2;
                // First try to fill up existing stacks
                for (size_t j = 0; j < invs[inv].slotCount; j++) {
                    if (!invs[inv].items[j].isEmpty && invs[inv].items[j].item.item.itemId == ids[i]) {
                        // Rechargeable items can only fill up empty slots, not existing stacks
                        if (invs[inv].items[j].item.item.itemId / 10000 != 207 && invs[inv].items[j].item.item.itemId / 10000 != 233) {
                            if (invs[inv].items[j].item.quantity > wz_get_item_info(ids[i])->slotMax - amounts[i]) {
                                amounts[i] -= wz_get_item_info(ids[i])->slotMax - invs[inv].items[j].item.quantity;
                                invs[inv].items[j].item.quantity = wz_get_item_info(ids[i])->slotMax;
                            } else {
                                invs[inv].items[j].item.quantity += amounts[i];
                                amounts[i] = 0;
                            }

                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mods[mod_count].quantity = invs[inv].items[j].item.quantity;
                            mod_count++;
                        }
                    }

                    if (amounts[i] == 0)
                        break;
                }

                // Then fill up empty slots
                if (amounts[i] != 0) {
                    for (size_t j = 0; j < invs[inv].slotCount; j++) {
                        if (invs[inv].items[j].isEmpty) {
                            invs[inv].items[j].isEmpty = false;
                            invs[inv].items[j].item.item.id = 0;
                            invs[inv].items[j].item.item.itemId = ids[i];
                            invs[inv].items[j].item.item.flags = 0;
                            invs[inv].items[j].item.item.ownerLength = 0;
                            invs[inv].items[j].item.item.giftFromLength = 0;
                            if (amounts[i] > wz_get_item_info(ids[i])->slotMax) {
                                invs[inv].items[j].item.quantity = wz_get_item_info(ids[i])->slotMax;
                                amounts[i] -= wz_get_item_info(ids[i])->slotMax;
                            } else {
                                invs[inv].items[j].item.quantity = amounts[i];
                                amounts[i] = 0;
                            }

                            if (mod_count == mod_capacity) {
                                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                                mod_capacity *= 2;
                            }

                            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_ADD;
                            mods[mod_count].inventory = inv + 2;
                            mods[mod_count].slot = j + 1;
                            mods[mod_count].item = invs[inv].items[j].item;
                            mod_count++;
                        }

                        if (amounts[i] == 0)
                            break;
                    }

                    if (amounts[i] > 0) {
                        free(mods);
                        return true;
                    }
                }
            }
        }
    }

    for (uint8_t i = 0; i < 4; i++)
        chr->inventory[i] = invs[i];

    for (uint8_t i = 0; i < chr->equipmentInventory.slotCount; i++) {
        chr->equipmentInventory.items[i].isEmpty = equip_inv.items[i].isEmpty;
        chr->equipmentInventory.items[i].equip = equip_inv.items[i].equip;
    }

    assert(mod_count != 0);
    while (mod_count > 255) {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(255, mods, packet);
        session_write(client->session, len, packet);
        mod_count -= 255;
    }

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(mod_count, mods, packet);
        session_write(client->session, len, packet);
    }

    if (reward) {
        for (size_t i = 0; i < len; i++) {
            uint8_t packet[ITEM_GAIN_IN_CHAT_PACKET_LENGTH];
            item_gain_in_chat_packet(ids[i], counts[i], packet);
            session_write(client->session, ITEM_GAIN_IN_CHAT_PACKET_LENGTH, packet);
        }
    }

    free(mods);
    *success = true;
    return true;
}

bool client_gain_inventory_item(struct Client *client, const struct InventoryItem *item, bool *success)
{
    struct Character *chr = &client->character;

    struct InventoryModify *mods = malloc(sizeof(struct InventoryModify));
    if (mods == NULL)
        return false;

    size_t mod_capacity = 1;
    size_t mod_count = 0;

    struct Inventory inv;
    inv = chr->inventory[item->item.itemId / 1000000 - 2];

    int16_t quantity = item->quantity;

    // First try to fill up existing stacks
    for (size_t j = 0; j < inv.slotCount; j++) {
        if (!inv.items[j].isEmpty && inv.items[j].item.item.itemId == item->item.itemId) {
            if (inv.items[j].item.quantity > wz_get_item_info(item->item.itemId)->slotMax - quantity) {
                quantity -= wz_get_item_info(item->item.itemId)->slotMax - inv.items[j].item.quantity;
                inv.items[j].item.quantity = wz_get_item_info(item->item.itemId)->slotMax;
            } else {
                inv.items[j].item.item.id = item->item.id;
                inv.items[j].item.quantity += quantity;
                quantity = 0;
            }

            if (mod_count == mod_capacity) {
                mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                mod_capacity *= 2;
            }

            mods[mod_count].mode = INVENTORY_MODIFY_TYPE_MODIFY;
            mods[mod_count].inventory = item->item.itemId / 1000000;
            mods[mod_count].slot = j + 1;
            mods[mod_count].quantity = inv.items[j].item.quantity;
            mod_count++;
        }

        if (quantity == 0)
            break;
    }

    // Then fill up an empty slot
    if (quantity != 0) {
        for (size_t j = 0; j < inv.slotCount; j++) {
            if (inv.items[j].isEmpty) {
                inv.items[j].isEmpty = false;
                inv.items[j].item.item = item->item;
                inv.items[j].item.quantity = quantity;

                if (mod_count == mod_capacity) {
                    mods = realloc(mods, (mod_capacity * 2) * sizeof(struct InventoryModify));
                    mod_capacity *= 2;
                }

                mods[mod_count].mode = INVENTORY_MODIFY_TYPE_ADD;
                mods[mod_count].inventory = item->item.itemId / 1000000;
                mods[mod_count].slot = j + 1;
                mods[mod_count].item = inv.items[j].item;
                mod_count++;

                chr->inventory[item->item.itemId / 1000000 - 2] = inv;

                while (mod_count > 255) {
                    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
                    size_t len = modify_items_packet(255, mods, packet);
                    session_write(client->session, len, packet);
                    mod_count -= 255;
                }

                {
                    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
                    size_t len = modify_items_packet(mod_count, mods, packet);
                    session_write(client->session, len, packet);
                }

                free(mods);
                *success = true;
                return true;
            }
        }
    } else {
        chr->inventory[item->item.itemId / 1000000 - 2] = inv;

        while (mod_count > 255) {
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(255, mods, packet);
            if (session_write(client->session, len, packet) == -1) {
                free(mods);
                return false;
            }
            mod_count -= 255;
        }

        {
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(mod_count, mods, packet);
            if (session_write(client->session, len, packet) == -1) {
                free(mods);
                return false;
            }
        }

        free(mods);
        *success = true;
        return true;
    }

    free(mods);
    *success = false;
    return true;
}

bool client_gain_equipment(struct Client *client, const struct Equipment *item, bool equip, bool *success)
{
    struct Character *chr = &client->character;
    if (!equip) {
        for (size_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
            if (chr->equipmentInventory.items[j].isEmpty) {
                chr->equipmentInventory.items[j].isEmpty = false;
                chr->equipmentInventory.items[j].equip = *item;

                struct InventoryModify mod = {
                    .mode = INVENTORY_MODIFY_TYPE_ADD,
                    .inventory = 1,
                    .slot = j + 1,
                    .equip = chr->equipmentInventory.items[j].equip
                };

                {
                    uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
                    size_t len = modify_items_packet(1, &mod, packet);
                    if (session_write(client->session, len, packet) == -1)
                        return false;
                }

                *success = true;
                return true;
            }
        }
    }

    // TODO: Handle immediate equip
    *success = false;
    return true;
}

bool client_remove_item(struct Client *client, uint8_t inventory, uint8_t src, int16_t amount, bool *success, struct InventoryItem *item)
{
    struct Character *chr = &client->character;
    if (inventory < 2 || inventory > 5)
        return false;

    inventory -= 2;
    src--;
    if (src >= chr->inventory[inventory].slotCount)
        return false;

    *success = false;
    if (chr->inventory[inventory].items[src].isEmpty)
        return true;

    if (chr->inventory[inventory].items[src].item.quantity < amount)
        return true;

    *item = chr->inventory[inventory].items[src].item;
    item->quantity = amount;
    if (chr->inventory[inventory].items[src].item.quantity == amount) {
        chr->inventory[inventory].items[src].isEmpty = true;
    } else {
        item->item.id = 0;
        chr->inventory[inventory].items[src].item.quantity -= amount;
    }

    {
        struct InventoryModify mod;
        mod.inventory = inventory + 2;
        mod.slot = src + 1;
        if (chr->inventory[inventory].items[src].isEmpty) {
            mod.mode = INVENTORY_MODIFY_TYPE_REMOVE;
        } else {
            mod.mode = INVENTORY_MODIFY_TYPE_MODIFY;
            mod.quantity = chr->inventory[inventory].items[src].item.quantity;
        }
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    *success = true;
    return true;
}

bool client_remove_equip(struct Client *client, bool equipped, uint8_t src, bool *success, struct Equipment *equip)
{
    struct Character *chr = &client->character;

    if (equipped) {
        src = equip_slot_to_compact(src);
        if (src >= EQUIP_SLOT_COUNT)
            return false;

        *success = false;
        if (chr->equippedEquipment[src].isEmpty)
            return true;

        *equip = chr->equippedEquipment[src].equip;
        chr->equippedEquipment[src].isEmpty = true;

        {
            struct InventoryModify mod;
            mod.inventory = 1;
            mod.slot = -equip_slot_from_compact(src);
            mod.mode = INVENTORY_MODIFY_TYPE_REMOVE;
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(1, &mod, packet);
            session_write(client->session, len, packet);
        }
    } else {
        src--;
        if (src >= chr->equipmentInventory.slotCount)
            return false;

        *success = false;
        if (chr->equipmentInventory.items[src].isEmpty)
            return true;

        *equip = chr->equipmentInventory.items[src].equip;
        chr->equipmentInventory.items[src].isEmpty = true;

        {
            struct InventoryModify mod;
            mod.inventory = 1;
            mod.slot = src + 1;
            mod.mode = INVENTORY_MODIFY_TYPE_REMOVE;
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(1, &mod, packet);
            session_write(client->session, len, packet);
        }
    }

    *success = true;
    return true;
}

bool client_move_item(struct Client *client, uint8_t inventory, uint8_t src, uint8_t dst)
{
    struct Character *chr = &client->character;
    if (inventory < 1 || inventory > 5)
        return false;

    src--;
    dst--;
    if (inventory != 1) {
        inventory -= 2;

        if (src >= chr->inventory[inventory].slotCount || dst >= chr->inventory[inventory].slotCount)
            return false;

        if (chr->inventory[inventory].items[src].isEmpty)
            return true;

        uint8_t mod_count;
        struct InventoryModify mods[2];
        if (chr->inventory[inventory].items[dst].isEmpty) {
            // Destination is empty - move the whole stack there
            chr->inventory[inventory].items[dst] = chr->inventory[inventory].items[src];
            chr->inventory[inventory].items[src].isEmpty = true;
            mods[0].mode = INVENTORY_MODIFY_TYPE_MOVE;
            mods[0].inventory = inventory + 2;
            mods[0].slot = src + 1;
            mods[0].dst = dst + 1;
            mod_count = 1;
        } else if (chr->inventory[inventory].items[dst].item.item.itemId == chr->inventory[inventory].items[src].item.item.itemId) {
            const struct ItemInfo *info = wz_get_item_info(chr->inventory[inventory].items[dst].item.item.itemId);
            if (chr->inventory[inventory].items[dst].item.quantity == info->slotMax) {
                // Destination is full with the same item - swap between the stacks
                struct InventoryItem temp = chr->inventory[inventory].items[dst].item;
                chr->inventory[inventory].items[dst].item = chr->inventory[inventory].items[src].item;
                chr->inventory[inventory].items[src].item = temp;
                mods[0].mode = INVENTORY_MODIFY_TYPE_MOVE;
                mods[0].inventory = inventory + 2;
                mods[0].slot = src + 1;
                mods[0].dst = dst + 1;
                mod_count = 1;
            } else {
                int16_t amount = info->slotMax - chr->inventory[inventory].items[dst].item.quantity;
                if (chr->inventory[inventory].items[src].item.quantity < amount) {
                    // Destination isn't full and it can consume the whole source - remove the source and modify the destination's item count
                    chr->inventory[inventory].items[dst].item.quantity += chr->inventory[inventory].items[src].item.quantity;
                    chr->inventory[inventory].items[src].isEmpty = true;
                    mods[0].mode = INVENTORY_MODIFY_TYPE_REMOVE;
                    mods[0].inventory = inventory + 2;
                    mods[0].slot = src + 1;
                    mods[1].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                    mods[1].inventory = inventory + 2;
                    mods[1].slot = dst + 1;
                    mods[1].quantity = chr->inventory[inventory].items[dst].item.quantity;
                    mod_count = 2;
                } else {
                    // Destination isn't full and it can't consume the whole source - modify both the source and the destination's item count
                    chr->inventory[inventory].items[dst].item.quantity += amount;
                    chr->inventory[inventory].items[src].item.quantity -= amount;
                    mods[0].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                    mods[0].inventory = inventory + 2;
                    mods[0].slot = src + 1;
                    mods[0].quantity = chr->inventory[inventory].items[src].item.quantity;
                    mods[1].mode = INVENTORY_MODIFY_TYPE_MODIFY;
                    mods[1].inventory = inventory + 2;
                    mods[1].slot = dst + 1;
                    mods[1].quantity = chr->inventory[inventory].items[dst].item.quantity;
                    mod_count = 2;
                }
            }
        } else {
            // Destination has a different item - swap them
            struct InventoryItem temp = chr->inventory[inventory].items[dst].item;
            chr->inventory[inventory].items[dst].item = chr->inventory[inventory].items[src].item;
            chr->inventory[inventory].items[src].item = temp;
            mods[0].mode = INVENTORY_MODIFY_TYPE_MOVE;
            mods[0].inventory = inventory + 2;
            mods[0].slot = src + 1;
            mods[0].dst = dst + 1;
            mod_count = 1;
        }

        {
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(mod_count, mods, packet);
            session_write(client->session, len, packet);
        }

        return true;
    } else {
        if (src >= chr->equipmentInventory.slotCount || dst >= chr->equipmentInventory.slotCount)
            return false;

        if (chr->equipmentInventory.items[src].isEmpty)
            return true;

        struct InventoryModify mod;
        if (chr->equipmentInventory.items[dst].isEmpty) {
            chr->equipmentInventory.items[dst] = chr->equipmentInventory.items[src];
            chr->equipmentInventory.items[src].isEmpty = true;
            mod.mode = INVENTORY_MODIFY_TYPE_MOVE;
            mod.inventory = 1;
            mod.slot = src + 1;
            mod.dst = dst + 1;
        } else {
            struct InventoryItem temp = chr->inventory[inventory].items[dst].item;
            chr->inventory[inventory].items[dst].item = chr->inventory[inventory].items[src].item;
            chr->inventory[inventory].items[src].item = temp;
            mod.mode = INVENTORY_MODIFY_TYPE_MOVE;
            mod.inventory = 1;
            mod.slot = src + 1;
            mod.dst = dst + 1;
        }

        {
            uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
            size_t len = modify_items_packet(1, &mod, packet);
            session_write(client->session, len, packet);
        }

        return true;

    }
}

bool client_equip(struct Client *client, uint8_t src, enum EquipSlot slot)
{
    // TODO: Check if there is enough room in the equipment inventory when both a top and a bottom are equipped and the player tries to equip an overall
    // or when both a weapon and a shield are equipped and the player tries to equip a two-handed weapon
    struct Character *chr = &client->character;

    if (!is_valid_equip_slot(slot))
        return false;

    uint8_t dst = equip_slot_to_compact(slot);
    src--;
    if (src >= chr->equipmentInventory.slotCount)
        return false;

    if (chr->equipmentInventory.items[src].isEmpty)
        return true;

    if (chr->equippedEquipment[dst].isEmpty) {
        chr->equippedEquipment[dst].isEmpty = false;
        chr->equippedEquipment[dst].equip = chr->equipmentInventory.items[src].equip;
        chr->estr += chr->equippedEquipment[dst].equip.str;
        chr->edex += chr->equippedEquipment[dst].equip.dex;
        chr->eint += chr->equippedEquipment[dst].equip.int_;
        chr->eluk += chr->equippedEquipment[dst].equip.luk;
        chr->eMaxHp += chr->equippedEquipment[dst].equip.hp;
        chr->eMaxMp += chr->equippedEquipment[dst].equip.mp;
        chr->equipmentInventory.items[src].isEmpty = true;
    } else {
        chr->estr -= chr->equippedEquipment[dst].equip.str;
        chr->edex -= chr->equippedEquipment[dst].equip.dex;
        chr->eint -= chr->equippedEquipment[dst].equip.int_;
        chr->eluk -= chr->equippedEquipment[dst].equip.luk;
        chr->eMaxHp -= chr->equippedEquipment[dst].equip.hp;
        chr->eMaxMp -= chr->equippedEquipment[dst].equip.mp;

        chr->estr += chr->equipmentInventory.items[src].equip.str;
        chr->edex += chr->equipmentInventory.items[src].equip.dex;
        chr->eint += chr->equipmentInventory.items[src].equip.int_;
        chr->eluk += chr->equipmentInventory.items[src].equip.luk;
        chr->eMaxHp += chr->equipmentInventory.items[src].equip.hp;
        chr->eMaxMp += chr->equipmentInventory.items[src].equip.mp;

        struct Equipment temp = chr->equipmentInventory.items[src].equip;
        chr->equipmentInventory.items[src].equip = chr->equippedEquipment[dst].equip;
        chr->equippedEquipment[dst].equip = temp;
    }

    struct InventoryModify mod = {
        .mode = INVENTORY_MODIFY_TYPE_MOVE,
        .inventory = 1,
        .slot = src + 1,
        .dst = -slot
    };

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    return true;
}

bool client_unequip(struct Client *client, enum EquipSlot slot, uint8_t dst)
{
    // TODO: Also unequip the bottom if the player tries to equip an overall instead of a shirt
    // TODO: Check if all equipment can still be worn after the stat change
    struct Character *chr = &client->character;
    if (!is_valid_equip_slot(slot))
        return false;

    uint8_t src = equip_slot_to_compact(slot);
    dst--;
    if (dst >= chr->equipmentInventory.slotCount)
        return false;

    if (chr->equipmentInventory.items[dst].isEmpty) {
        chr->estr -= chr->equippedEquipment[src].equip.str;
        chr->edex -= chr->equippedEquipment[src].equip.dex;
        chr->eint -= chr->equippedEquipment[src].equip.int_;
        chr->eluk -= chr->equippedEquipment[src].equip.luk;
        chr->eMaxHp -= chr->equippedEquipment[src].equip.hp;
        chr->eMaxMp -= chr->equippedEquipment[src].equip.mp;

        chr->equipmentInventory.items[dst].isEmpty = false;
        chr->equipmentInventory.items[dst].equip = chr->equippedEquipment[src].equip;
        chr->equippedEquipment[src].isEmpty = true;
    } else {
        chr->estr -= chr->equippedEquipment[src].equip.str;
        chr->edex -= chr->equippedEquipment[src].equip.dex;
        chr->eint -= chr->equippedEquipment[src].equip.int_;
        chr->eluk -= chr->equippedEquipment[src].equip.luk;
        chr->eMaxHp -= chr->equippedEquipment[src].equip.hp;
        chr->eMaxMp -= chr->equippedEquipment[src].equip.mp;

        chr->estr += chr->equipmentInventory.items[dst].equip.str;
        chr->edex += chr->equipmentInventory.items[dst].equip.dex;
        chr->eint += chr->equipmentInventory.items[dst].equip.int_;
        chr->eluk += chr->equipmentInventory.items[dst].equip.luk;
        chr->eMaxHp += chr->equipmentInventory.items[dst].equip.hp;
        chr->eMaxMp += chr->equipmentInventory.items[dst].equip.mp;

        struct Equipment temp = chr->equippedEquipment[src].equip;
        chr->equippedEquipment[src].equip = chr->equipmentInventory.items[dst].equip;
        chr->equipmentInventory.items[dst].equip = temp;
    }

    enum Stat stats = 0;
    union StatValue values[2];
    if (chr->hp > character_get_effective_hp(chr)) {
        chr->hp = character_get_effective_hp(chr);
        values[0].i16 = chr->hp;
        stats |= STAT_HP;
    }

    if (chr->mp > character_get_effective_mp(chr)) {
        chr->mp = character_get_effective_mp(chr);
        values[stats != 0 ? 1 : 0].i16 = chr->mp;
        stats |= STAT_MP;
    }

    if (stats != 0) {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, stats, values, packet);
        session_write(client->session, len, packet);
    }

    struct InventoryModify mod = {
        .mode = INVENTORY_MODIFY_TYPE_MOVE,
        .inventory = 1,
        .slot = -slot,
        .dst = dst + 1,
    };

    {
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    return true;
}

bool client_use_item(struct Client *client, uint8_t slot, uint32_t id)
{
    struct Character *chr = &client->character;
    slot--;
    if (slot >= chr->inventory[0].slotCount)
        return false;

    // TODO: Check if id is a usable item

    const struct ConsumableInfo *info = wz_get_consumable_info(id);
    if (info == NULL)
        return false;

    if (chr->inventory[0].items[slot].isEmpty || chr->inventory[0].items[slot].item.item.itemId != id)
        return true;

    enum Stat stats = 0;
    size_t value_count = 0;
    union StatValue values[2];

    if (info->hp != 0 || info->hpR != 0) {
        int16_t hp = info->hp + character_get_effective_hp(chr) * info->hpR / 100; // TODO: This addition can overflow
        if (chr->hp > character_get_effective_hp(chr) - hp)
            hp = character_get_effective_hp(chr) - chr->hp;

        chr->hp += hp;

        values[value_count].i16 = chr->hp,
        stats |= STAT_HP;
        value_count++;
    }

    if (info->mp != 0 || info->mpR != 0) {
        int16_t mp = info->mp + character_get_effective_mp(chr) * info->mpR / 100; // TODO: This addition can overflow
        if (chr->mp > character_get_effective_mp(chr) - mp)
            mp = character_get_effective_mp(chr) - chr->mp;

        chr->mp += mp;

        values[value_count].i16 = chr->mp;
        stats |= STAT_MP;
        value_count++;
    }

    // Also enables actions if value_count is 0
    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, stats, values, packet);
        session_write(client->session, len, packet);
    }

    chr->inventory[0].items[slot].item.quantity--;
    if (chr->inventory[0].items[slot].item.quantity == 0) {
        chr->inventory[0].items[slot].isEmpty = true;
        struct InventoryModify mod = {
            .mode = INVENTORY_MODIFY_TYPE_REMOVE,
            .inventory = 2,
            .slot = slot + 1,
        };
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    } else {
        struct InventoryModify mod = {
            .mode = INVENTORY_MODIFY_TYPE_MODIFY,
            .inventory = 2,
            .slot = slot + 1,
            .quantity = chr->inventory[0].items[slot].item.quantity
        };
        uint8_t packet[MODIFY_ITEMS_PACKET_MAX_LENGTH];
        size_t len = modify_items_packet(1, &mod, packet);
        session_write(client->session, len, packet);
    }

    return true;
}

bool client_use_item_immediate(struct Client *client, uint32_t id)
{
    struct Character *chr = &client->character;

    const struct ConsumableInfo *info = wz_get_consumable_info(id);
    if (info == NULL)
        return false;

    //apply_effects(client, info)
    enum Stat stats = 0;
    size_t value_count = 0;
    union StatValue values[2];

    if (info->hp != 0 || info->hpR != 0) {
        int16_t hp = info->hp + character_get_effective_hp(chr) * info->hpR / 100; // TODO: This addition can overflow
        if (chr->hp > character_get_effective_hp(chr) - hp)
            hp = character_get_effective_hp(chr) - chr->hp;

        chr->hp += hp;

        values[value_count].i16 = chr->hp,
        stats |= STAT_HP;
        value_count++;
    }

    if (info->mp != 0 || info->mpR != 0) {
        int16_t mp = info->mp + character_get_effective_mp(chr) * info->mpR / 100; // TODO: This addition can overflow
        if (chr->mp > character_get_effective_mp(chr) - mp)
            mp = character_get_effective_mp(chr) - chr->mp;

        chr->mp += mp;

        values[value_count].i16 = chr->mp;
        stats |= STAT_MP;
        value_count++;
    }

    if (wz_get_item_info(id)->monsterBook) {
        struct MonsterBookEntry *entry = hash_set_u32_get(chr->monsterBook, id);
        if (entry == NULL || entry->count < 5) {
            if (entry == NULL) {
                    struct MonsterBookEntry new = {
                    .id = id,
                    .count = 0
                };
                hash_set_u32_insert(chr->monsterBook, &new);
                entry = hash_set_u32_get(chr->monsterBook, id);
            }

            entry->count++;

            {
                uint8_t packet[ADD_CARD_PACKET_LENGTH];
                add_card_packet(false, id, entry->count, packet);
                session_write(client->session, ADD_CARD_PACKET_LENGTH, packet);
            }

            {
                uint8_t packet[SHOW_EFFECT_PACKET_LENGTH];
                show_effect_packet(0x0D, packet);
                session_write(client->session, SHOW_EFFECT_PACKET_LENGTH, packet);
            }
        } else {
            uint8_t packet[ADD_CARD_PACKET_LENGTH];
            add_card_packet(true, id, 5, packet);
            session_write(client->session, ADD_CARD_PACKET_LENGTH, packet);
        }

        // Show monster card effect to other players regardless if the card slot is full or not
        {
            uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
            show_foreign_effect_packet(chr->id, 0x0D, packet);
            session_write(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
        }
    }

    // Also enables actions if value_count is 0
    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        size_t len = stat_change_packet(true, stats, values, packet);
        session_write(client->session, len, packet);
    }

    return true;
}

bool client_is_quest_started(struct Client *client, uint16_t qid)
{
    return hash_set_u16_get(client->character.quests, qid) != NULL;
}

struct ClientResult client_npc_talk(struct Client *client, uint32_t npc)
{
    if (client->script != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    client->npc = npc;
    char script[12];
    snprintf(script, 12, "%d.lua", npc);
    client->script = script_manager_alloc(client->managers.npc, script, 0);
    if (client->script == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    struct ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
    switch (res.result) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
    case SCRIPT_RESULT_VALUE_NEXT:
    break;
    case SCRIPT_RESULT_VALUE_WARP:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_WARP, .map = res.value.i, .portal = res.value2.i };
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

struct ClientResult client_start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted)
{
    struct Character *chr = &client->character;

    if (scripted && client->script != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    const struct QuestInfo *info = wz_get_quest_info(qid);
    if (info == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if (hash_set_u16_get(chr->quests, qid) != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    // TODO: Also check if this quest isn't repeatable
    if (hash_set_u16_get(chr->completedQuests, qid) != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if ((scripted && !info->startScript) || (!scripted && info->startScript))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if (!check_quest_requirements(chr, info->startRequirementCount, info->startRequirements, npc))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if (info->startScript) {
        client->npc = npc;
        client->qid = qid;
        char script[10];
        snprintf(script, 10, "%d.lua", qid);
        client->script = script_manager_alloc(client->managers.quest, script, 0);
        struct ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
        switch (res.result) {
        case SCRIPT_RESULT_VALUE_KICK:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };
        case SCRIPT_RESULT_VALUE_FAILURE:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
        case SCRIPT_RESULT_VALUE_SUCCESS:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        case SCRIPT_RESULT_VALUE_NEXT:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        case SCRIPT_RESULT_VALUE_WARP:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_WARP, .map = res.value.i, .portal = res.value2.i };
        }
    }

    bool success;
    if (!start_quest(client, qid, npc, &success))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

    if (!success) {
        uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
        char *message = "Please check if you have enough space in your inventory.";
        size_t len = popup_message_packet(strlen(message), message, packet);
        session_write(client->session, len, packet);
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

bool client_start_quest_now(struct Client *client, bool *success)
{
    return start_quest(client, client->qid, client->npc, success);
}

struct ClientResult client_end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool scripted)
{
    struct Character *chr = &client->character;

    if (scripted && client->script != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    const struct QuestInfo *info = wz_get_quest_info(qid);
    if (info == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if (hash_set_u16_get(chr->completedQuests, qid) != NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };

    if (hash_set_u16_get(chr->quests, qid) == NULL)
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if ((scripted && !info->endScript) || (!scripted && info->endScript))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if (!check_quest_requirements(chr, info->endRequirementCount, info->endRequirements, npc))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };

    if (info->endScript) {
        client->npc = npc;
        client->qid = qid;
        char script[10];
        snprintf(script, 10, "%d.lua", qid);
        client->script = script_manager_alloc(client->managers.quest, script, 1);
        struct ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
        switch (res.result) {
        case SCRIPT_RESULT_VALUE_KICK:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };
        case SCRIPT_RESULT_VALUE_FAILURE:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
        case SCRIPT_RESULT_VALUE_SUCCESS:
            script_manager_free(client->script);
            client->script = NULL;
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        case SCRIPT_RESULT_VALUE_NEXT:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
        case SCRIPT_RESULT_VALUE_WARP:
            return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_WARP, .map = res.value.i, .portal = res.value2.i };
        }
    }

    bool success;
    if (!end_quest(client, qid, npc, &success))
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };

    if (!success) {
        uint8_t packet[POPUP_MESSAGE_PACKET_MAX_LENGTH];
        char *message = "Please check if you have enough space in your inventory.";
        size_t len = popup_message_packet(strlen(message), message, packet);
        session_write(client->session, len, packet);
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

bool client_end_quest_now(struct Client *client, bool *success)
{
    return end_quest(client, client->qid, client->npc, success);
}

bool client_forfeit_quest(struct Client *client, uint16_t qid)
{
    struct Quest *quest = hash_set_u16_get(client->character.quests, qid);
    if (quest == NULL)
        return true;

    const struct QuestInfo *info = wz_get_quest_info(qid);
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        struct QuestRequirement *req = &info->endRequirements[i];
        if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
            for (uint8_t i = 0; i < quest->progressCount; i++) {
                struct Progress *progress = &quest->progress[i];
                for (uint8_t i = 0; i < req->mob.count; i++) {
                    if (progress->id == req->mob.mobs[i].id) {
                        if (progress->amount < req->mob.mobs[i].count) {
                            struct MonsterRefCount *m = hash_set_u32_get(client->character.monsterQuests, progress->id);
                            m->refCount--;
                            if (m->refCount == 0)
                                hash_set_u32_remove(client->character.monsterQuests, m->id);
                        }
                        break;
                    }
                }
            }
            break;
        }
    }

    hash_set_u16_remove(client->character.quests, qid);

    {
        uint8_t packet[FORFEIT_QUEST_PACKET_LENGTH];
        forfeit_quest_packet(qid, packet);
        session_write(client->session, FORFEIT_QUEST_PACKET_LENGTH, packet);
    }

    return true;
}

struct ClientResult client_script_cont(struct Client *client, uint32_t action)
{
    struct ScriptResult res;
    if (client->scriptState != SCRIPT_STATE_WARP && action == -1) {
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    }

    switch (client->scriptState) {
    case SCRIPT_STATE_OK:
        res = script_manager_run(client->script, 1);
    break;
    case SCRIPT_STATE_YES_NO:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_SIMPLE:
        res = script_manager_run(client->script, action);
    break;
    case SCRIPT_STATE_NEXT:
        res = script_manager_run(client->script, 1);
    break;
    case SCRIPT_STATE_PREV_NEXT:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_PREV:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_ACCEPT_DECILNE:
        res = script_manager_run(client->script, action == 0 ? -1 : 1);
    break;
    case SCRIPT_STATE_WARP:
        res = script_manager_run(client->script, 0);
    break;
    }

    switch (res.result) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
    case SCRIPT_RESULT_VALUE_NEXT:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
    case SCRIPT_RESULT_VALUE_WARP:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_WARP, .map = res.value.i, .portal = res.value2.i };
    }
}

struct CheckProgressContext {
    struct Session *session;
    struct HashSetU32 *monsterQuests;
    uint32_t id;
};

static void check_progress(void *data, void *ctx);

void client_kill_monster(struct Client *client, uint32_t id)
{
    struct Character *chr = &client->character;
    const struct MobInfo *info = wz_get_monster_stats(id);
    client_gain_exp(client, info->exp, false);
    struct CheckProgressContext ctx = {
        .session = client->session,
        .monsterQuests = chr->monsterQuests,
        .id = id
    };

    if (hash_set_u32_get(client->character.monsterQuests, id) != NULL)
        hash_set_u16_foreach(chr->quests, check_progress, &ctx);
}

void client_send_ok(struct Client *client, size_t msg_len, const char *msg)
{
    client->scriptState = SCRIPT_STATE_OK;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_OK, msg_len, msg, packet);
    session_write(client->session, len, packet);
}

void client_send_yes_no(struct Client *client, size_t msg_len, const char *msg)
{
    client->scriptState = SCRIPT_STATE_YES_NO;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_YES_NO, msg_len, msg, packet);
    session_write(client->session, len, packet);
}

void client_send_simple(struct Client *client, size_t msg_len, const char *msg)
{
    client->scriptState = SCRIPT_STATE_SIMPLE;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_SIMPLE, msg_len, msg, packet);
    session_write(client->session, len, packet);
}

void client_send_next(struct Client *client, size_t msg_len, const char *msg)
{
    client->scriptState = SCRIPT_STATE_NEXT;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_NEXT, msg_len, msg, packet);
    session_write(client->session, len, packet);
}

void client_send_prev_next(struct Client *client, size_t msg_len, const char *msg)
{
    client->scriptState = SCRIPT_STATE_PREV_NEXT;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_PREV_NEXT, msg_len, msg, packet);
    session_write(client->session, len, packet);
}

void client_send_prev(struct Client *client, size_t msg_len, const char *msg)
{
    client->scriptState = SCRIPT_STATE_PREV;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_PREV, msg_len, msg, packet);
    session_write(client->session, len, packet);
}

void client_send_accept_decline(struct Client *client, size_t msg_len, const char *msg)
{
    client->scriptState = SCRIPT_STATE_ACCEPT_DECILNE;
    uint8_t packet[NPC_DIALOGUE_PACKET_MAX_LENGTH];
    size_t len = npc_dialogue_packet(client->npc, NPC_DIALOGUE_TYPE_ACCEPT_DECILNE, msg_len, msg, packet);
    session_write(client->session, len, packet);
}

void client_warp(struct Client *client, uint32_t map, uint8_t portal)
{
    struct Character *chr = &client->character;
    client->scriptState = SCRIPT_STATE_WARP;
    client->character.map = map;
    client->character.spawnPoint = portal;
    {
        uint8_t packet[REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH];
        remove_player_from_map_packet(chr->id, packet);
        session_broadcast_to_room(client->session, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);
    }
    map_leave(room_get_context(session_get_room(client->session)), client->map.handle);
    client->map.handle = NULL;

    {
        uint8_t packet[CHANGE_MAP_PACKET_LENGTH];
        change_map_packet(&client->character, client->character.map, client->character.spawnPoint, packet);
        session_write(client->session, CHANGE_MAP_PACKET_LENGTH, packet);
    }
}

void client_reset_stats(struct Client *client)
{
    struct Character *chr = &client->character;
    chr->ap += chr->str + chr->int_ + chr->dex + chr->luk - 16;
    chr->str = 4;
    chr->dex = 4;
    chr->int_ = 4;
    chr->luk = 4;
    chr->sp = 1;

    switch (chr->job) {
        case JOB_SWORDSMAN:
        case JOB_DAWN_WARRIOR:
        case JOB_ARAN:
            chr->ap -= 35 - chr->str;
            chr->str = 35;
            chr->sp += (chr->level - 10) * 3;
        break;

        case JOB_MAGICIAN:
        case JOB_BLAZE_WIZARD:
            chr->ap -= 20 - chr->int_;
            chr->int_ = 20;
            chr->sp += (chr->level - 8) * 3;
        break;

        case JOB_ARCHER:
        case JOB_WIND_ARCHER:
        case JOB_ROGUE:
        case JOB_NIGHT_WALKER:
            chr->ap -= 25 - chr->dex;
            chr->dex = 25;
            chr->sp += (chr->level - 10) * 3;
        break;

        case JOB_PIRATE:
        case JOB_THUNDER_BREAKER:
            chr->ap -= 20 - chr->dex;
            chr->dex = 20;
            chr->sp += (chr->level - 10) * 3;
        break;

        default:
        break;
    }

    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    union StatValue values[] = {
        {
            .i16 = chr->str
        },
        {
            .i16 = chr->dex
        },
        {
            .i16 = chr->int_
        },
        {
            .i16 = chr->luk
        },
        {
            .i16 = chr->ap
        },
        {
            .i16 = chr->sp
        },
    };
    size_t len = stat_change_packet(true, STAT_STR | STAT_DEX | STAT_INT | STAT_LUK | STAT_AP | STAT_SP, values, packet);
    session_write(client->session, len, packet);
}

void client_change_job(struct Client *client, enum Job job)
{
    struct Character *chr = &client->character;

    chr->job = job;
    int16_t sp = 1;
    if (job % 10 == 2)
        sp += 2;

    client_adjust_sp(client, sp);

    // TODO: Gain inventory slots

    int16_t rand1 = random();
    if (chr->job % 1000 == 100) {
        rand1 = rand1 % (250 - 200 + 1) + 200;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        chr->maxHp += rand1;
    } else if (chr->job % 1000 == 200) {
        rand1 = rand1 % (150 - 100 + 1) + 100;
        if (chr->maxMp > 30000 - rand1)
            rand1 = 30000 - chr->maxMp;
        chr->maxMp += rand1;
    } else if (chr->job % 100 == 0) {
        rand1 = rand1 % (150 - 100 + 1) + 100;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        chr->maxHp += rand1;

        int16_t rand2 = random() % (50 - 25 + 1) + 25;
        if (chr->maxMp > 30000 - rand2)
            rand2 = 30000 - chr->maxMp;
        chr->maxMp += rand2;
    } else if (chr->job % 1000 > 0 && chr->job % 1000 < 200) {
        rand1 = rand1 % (350 - 300 + 1) + 300;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        chr->maxHp += rand1;
    } else if (chr->job % 1000 < 300) {
        rand1 = rand1 % (500 - 450 + 1) + 450;
        if (chr->maxMp > 30000 - rand1)
            rand1 = 30000 - chr->maxMp;
        chr->maxMp += rand1;
    } else if (chr->job % 1000 > 300) {
        rand1 = rand1 % (350 - 300 + 1) + 300;
        if (chr->maxHp > 30000 - rand1)
            rand1 = 30000 - chr->maxHp;
        chr->maxHp += rand1;

        int16_t rand2 = random() % (200 - 150 + 1) + 150;
        if (chr->maxMp > 30000 - rand2)
            rand2 = 30000 - chr->maxMp;
        chr->maxMp += rand2;
    }

    {
        uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
        union StatValue values[] = {
            {
                .u16 = chr->job
            },
            {
                .i16 = chr->maxHp
            },
            {
                .i16 = chr->maxMp
            }
        };
        size_t len = stat_change_packet(true, STAT_JOB | STAT_MAX_HP | STAT_MAX_MP, values, packet);
        session_write(client->session, len, packet);
    }

    // TODO: Broadcast to family, party, guild

    //{
    //    uint8_t packet[REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH];
    //    remove_player_from_map_packet(chr->id, packet);
    //    session_broadcast_to_room(client->session.session, REMOVE_PLAYER_FROM_MAP_PACKET_LENGTH, packet);
    //}

    //{
    //    uint8_t packet[ADD_PLAYER_TO_MAP_PACKET_MAX_LENGTH];
    //    size_t len = add_player_to_map_packet(chr, false, packet);
    //    session_broadcast_to_room(client->session.session, len, packet);
    //}

    {
        uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
        show_foreign_effect_packet(chr->id, 8, packet);
        session_broadcast_to_room(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
    }
}

struct ClientResult client_portal_script(struct Client *client, const char *portal)
{
    char script[21];
    snprintf(script, 21, "%s.lua", portal);
    client->script = script_manager_alloc(client->managers.portal, script, 0);
    struct ScriptResult res = script_manager_run(client->script, SCRIPT_CLIENT_TYPE, client);
    switch (res.result) {
    case SCRIPT_RESULT_VALUE_KICK:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_KICK };
    case SCRIPT_RESULT_VALUE_FAILURE:
        script_manager_free(client->script);
        client->script = NULL;
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_ERROR };
    case SCRIPT_RESULT_VALUE_SUCCESS:
        script_manager_free(client->script);
        client->script = NULL;
    case SCRIPT_RESULT_VALUE_NEXT:
        break;
    case SCRIPT_RESULT_VALUE_WARP:
        return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_WARP, .map = res.value.i, .portal = res.value2.i };
    }

    return (struct ClientResult) { .type = CLIENT_RESULT_TYPE_SUCCESS };
}

void client_enable_actions(struct Client *client)
{
    uint8_t packet[STAT_CHANGE_PACKET_MAX_LENGTH];
    size_t len = stat_change_packet(true, 0, NULL, packet);
    session_write(client->session, len, packet);
}

static bool check_quest_requirements(struct Character *chr, size_t req_count, const struct QuestRequirement *reqs, uint32_t npc)
{
    for (size_t i = 0; i < req_count; i++) {
        switch (reqs[i].type) {
        case QUEST_REQUIREMENT_TYPE_NPC:
            if (reqs[i].npc.id != npc)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_QUEST:
            switch (reqs[i].quest.state) {
            case QUEST_STATE_NOT_STARTED:
                if (hash_set_u16_get(chr->quests, reqs[i].quest.id) != NULL)
                    return false;

                if (hash_set_u16_get(chr->completedQuests, reqs[i].quest.id) != NULL)
                    return false;
            break;

            case QUEST_STATE_STARTED:
                if (hash_set_u16_get(chr->quests, reqs[i].quest.id) == NULL)
                    return false;
            break;

            case QUEST_STATE_COMPLETED:
                if (hash_set_u16_get(chr->completedQuests, reqs[i].quest.id) == NULL)
                    return false;
            break;
            }
        break;

        case QUEST_REQUIREMENT_TYPE_JOB: {
            size_t j;
            for (j = 0; j < reqs[i].job.count; j++) {
                if (reqs[i].job.jobs[j] == chr->job)
                    break;
            }

            if (j == reqs[i].job.count)
                return false;
        }
        break;

        case QUEST_REQUIREMENT_TYPE_COMPLETED_QUEST:
            if (hash_set_u16_size(chr->completedQuests) < reqs[i].questCompleted.amount)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_MESO:
            if (chr->mesos < reqs[i].meso.amount)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_MIN_LEVEL:
            if (chr->level < reqs[i].minLevel.level)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_MAX_LEVEL:
            if (chr->level > reqs[i].maxLevel.level)
                return false;
        break;

        case QUEST_REQUIREMENT_TYPE_ITEM: {
            enum InventoryType inv = item_get_inventory(reqs[i].item.id);
            if (reqs[i].item.count != 0) {
                int32_t amount = 0;
                if (inv == INVENTORY_TYPE_EQUIP) {
                    for (size_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
                        if (!chr->equipmentInventory.items[j].isEmpty && chr->equipmentInventory.items[j].equip.item.itemId == reqs[i].item.id)
                            amount++;
                    }

                    // TODO: Check in the equipped equipment

                    if (amount < reqs[i].item.count)
                        return false;
                } else {
                    for (size_t j = 0; j < chr->inventory[inv - 1].slotCount; j++) {
                        if (!chr->inventory[inv - 1].items[j].isEmpty && chr->inventory[inv - 1].items[j].item.item.itemId == reqs[i].item.id)
                            amount += chr->inventory[inv - 1].items[j].item.quantity;
                    }

                    if (amount < reqs[i].item.count)
                        return false;
                }
            } else {
                // if count is 0 this means that the player shouldn't have any of the item
                if (inv == INVENTORY_TYPE_EQUIP) {
                    for (size_t j = 0; j < chr->equipmentInventory.slotCount; j++) {
                        if (!chr->equipmentInventory.items[j].isEmpty && chr->equipmentInventory.items[j].equip.item.itemId == reqs[i].item.id)
                            return false;
                    }
                } else {
                    for (size_t j = 0; j < chr->inventory[inv - 1].slotCount; j++) {
                        if (!chr->inventory[inv - 1].items[j].isEmpty && chr->inventory[inv - 1].items[j].item.item.itemId == reqs[i].item.id)
                            return false;
                    }
                }

            }
        }
        break;
        }
    }

    return true;
}

static bool start_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success)
{
    bool success_;
    if (success == NULL)
        success = &success_;

    struct Character *chr = &client->character;

    const struct QuestInfo *info = wz_get_quest_info(qid);

    struct Quest quest = {
        .id = qid,
        .progressCount = 0
    };
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        struct QuestRequirement *req = &info->endRequirements[i];
        if (req->type == QUEST_REQUIREMENT_TYPE_MOB) {
            for (size_t i = 0; i < req->mob.count; i++) {
                struct MonsterRefCount *m = hash_set_u32_get(chr->monsterQuests, req->mob.mobs[i].id);
                if (m == NULL) {
                    struct MonsterRefCount new = {
                        .id = req->mob.mobs[i].id,
                        .refCount = 1
                    };
                    hash_set_u32_insert(chr->monsterQuests, &new);
                } else {
                    m->refCount++;
                }
                quest.progress[quest.progressCount].id = req->mob.mobs[i].id;
                quest.progress[quest.progressCount].amount = 0;
                quest.progressCount++;
            }
        }
    }

    if (hash_set_u16_insert(chr->quests, &quest) == -1)
        return false;

    *success = false;
    for (size_t i = 0; i < info->startActCount; i++) {
        switch (info->startActs[i].type) {
        case QUEST_ACT_TYPE_EXP:
            client_gain_exp(client, info->startActs[i].exp.amount, true);
        break;
        case QUEST_ACT_TYPE_MESO:
            client_gain_meso(client, info->startActs[i].meso.amount, false, true);
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
                size_t j;
                for (j = 0; j < chr->equipmentInventory.slotCount; j++)
                    if (chr->equipmentInventory.items[j].isEmpty)
                        break;

                if (j == chr->equipmentInventory.slotCount) {
                    hash_set_u16_remove(chr->quests, qid);
                    return true;
                }

                for (size_t i = 0; i < 4; i++) {
                    for (j = 0; j < chr->inventory[i].slotCount; j++) {
                        if (chr->inventory[i].items[j].isEmpty)
                            break;
                    }

                    if (j == chr->inventory[i].slotCount) {
                        hash_set_u16_remove(chr->quests, qid);
                        return true;
                    }
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

            bool success;
            if (!client_gain_items(client, item_count, ids, amounts, true, &success)) {
                hash_set_u16_remove(chr->quests, qid);
                return false;
            }

            if (!success) {
                hash_set_u16_remove(chr->quests, qid);
                return true;
            }
        }
        break;

        default:
            fprintf(stderr, "Unimplemented\n");
        }
    }

    char progress[15];
    size_t prog_len = quest_get_progress_string(&quest, progress);
    {
        uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
        size_t len = update_quest_packet(qid, prog_len, progress, packet);
        session_write(client->session, len, packet);
    }

    {
        uint8_t packet[START_QUEST_PACKET_LENGTH];
        start_quest_packet(qid, npc, packet);
        session_write(client->session, START_QUEST_PACKET_LENGTH, packet);
    }

    *success = true;
    return true;
}

static bool end_quest(struct Client *client, uint16_t qid, uint32_t npc, bool *success)
{
    bool success_;
    if (success == NULL)
        success = &success_;

    struct Character *chr = &client->character;

    struct CompletedQuest quest = {
        .id = qid,
        .time = time(NULL),
    };

    if (hash_set_u16_insert(chr->completedQuests, &quest) == -1)
        return false;

    {
        uint8_t packet[UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH];
        update_quest_completion_time_packet(qid, quest.time, packet);
        session_write(client->session, UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH, packet);
    }

    {
        uint8_t packet[SHOW_EFFECT_PACKET_LENGTH];
        show_effect_packet(0x09, packet);
        session_write(client->session, SHOW_EFFECT_PACKET_LENGTH, packet);
    }

    {
        uint8_t packet[SHOW_FOREIGN_EFFECT_PACKET_LENGTH];
        show_foreign_effect_packet(chr->id, 0x09, packet);
        session_broadcast_to_room(client->session, SHOW_FOREIGN_EFFECT_PACKET_LENGTH, packet);
    }

    *success = false;
    const struct QuestInfo *info = wz_get_quest_info(qid);
    bool next_quest = false;
    for (size_t i = 0; i < info->endActCount; i++) {
        switch (info->endActs[i].type) {
        case QUEST_ACT_TYPE_EXP:
            client_gain_exp(client, info->endActs[i].exp.amount, true);
        break;

        case QUEST_ACT_TYPE_MESO:
            client_gain_meso(client, info->endActs[i].meso.amount, false, true);
        break;

        case QUEST_ACT_TYPE_ITEM: {
            bool has_prop = false;
            int8_t item_count = 0;
            int32_t total = 0;
            for (size_t j = 0; j < info->endActs[i].item.count; j++) {
                if (info->endActs[i].item.items[j].prop == 0 || !has_prop) {
                    if (info->endActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    item_count++;
                }

                total += info->endActs[i].item.items[j].prop;
            }

            uint32_t ids[item_count];
            int16_t amounts[item_count];

            int32_t r;
            if (has_prop) {
                // If there is a random item, there should be at least one empty slot in each inventory
                size_t j;
                for (j = 0; j < chr->equipmentInventory.slotCount; j++)
                    if (chr->equipmentInventory.items[j].isEmpty)
                        break;

                if (j == chr->equipmentInventory.slotCount) {
                    hash_set_u16_remove(chr->completedQuests, qid);
                    return true;
                }

                for (size_t i = 0; i < 4; i++) {
                    for (j = 0; j < chr->inventory[i].slotCount; j++) {
                        if (chr->inventory[i].items[j].isEmpty)
                            break;
                    }

                    if (j == chr->inventory[i].slotCount) {
                        hash_set_u16_remove(chr->completedQuests, qid);
                        return true;
                    }
                }
                r = rand() % total;
            }

            has_prop = false;
            item_count = 0;
            total = 0;
            for (size_t j = 0; j < info->endActs[i].item.count; j++) {
                total += info->endActs[i].item.items[j].prop;
                if (info->endActs[i].item.items[j].prop == 0 || (!has_prop && r < total)) {
                    if (info->endActs[i].item.items[j].prop != 0)
                        has_prop = true;

                    ids[item_count] = info->endActs[i].item.items[j].id;
                    amounts[item_count] = info->endActs[i].item.items[j].count;
                    item_count++;
                }
            }

            bool succ;
            if (!client_gain_items(client, item_count, ids, amounts, true, &succ)) {
                hash_set_u16_remove(chr->completedQuests, qid);
                return false;
            }

            if (!succ) {
                hash_set_u16_remove(chr->completedQuests, qid);
                return true;
            }
        }
        break;

        case QUEST_ACT_TYPE_NEXT_QUEST: {
            next_quest = true;
            uint8_t packet[UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH];
            end_quest_packet(qid, npc, info->endActs[i].nextQuest.qid, packet);
            session_write(client->session, UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH, packet);
        }
        break;

        default:
            fprintf(stderr, "Unimplemented\n");
        }
    }

    if (!next_quest) {
        uint8_t packet[UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH];
        end_quest_packet(qid, npc, 0, packet);
        session_write(client->session, UPDATE_QUEST_COMPLETION_TIME_PACKET_LENGTH, packet);
    }

    hash_set_u16_remove(chr->quests, qid);

    *success = true;
    return true;
}

static void check_progress(void *data, void *ctx_)
{
    struct CheckProgressContext *ctx = ctx_;
    struct Quest *quest = data;
    const struct QuestInfo *info = wz_get_quest_info(quest->id);

    const struct QuestRequirement *req;
    for (size_t i = 0; i < info->endRequirementCount; i++) {
        if (info->endRequirements[i].type == QUEST_REQUIREMENT_TYPE_MOB) {
            req = &info->endRequirements[i];
            break;
        }
    }

    for (size_t i = 0; i < quest->progressCount; i++) {
        int16_t req_amount;
        for (uint8_t j = 0; j < req->mob.count; j++) {
            if (quest->progress[i].id == req->mob.mobs[j].id) {
                req_amount = req->mob.mobs[j].count;
                break;
            }
        }

        if (quest->progress[i].id == ctx->id) {
            quest->progress[i].amount++;
            if (quest->progress[i].amount == req_amount) {
                struct MonsterRefCount *monster = hash_set_u32_get(ctx->monsterQuests, ctx->id);
                monster->refCount--;
                if (monster->refCount == 0)
                    hash_set_u32_remove(ctx->monsterQuests, ctx->id);
            }

            char progress[15];
            size_t prog_len = quest_get_progress_string(quest, progress);
            uint8_t packet[UPDATE_QUEST_PACKET_MAX_LENGTH];
            size_t len = update_quest_packet(quest->id, prog_len, progress, packet);
            session_write(ctx->session, len, packet);
        }
    }
}


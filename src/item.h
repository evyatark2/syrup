#ifndef ITEM_H
#define ITEM_H

#include <stdbool.h>
#include <stdint.h>

#include "wz.h"

#define EQUIP_SLOT_COUNT 64
#define EQUIP_SLOT_NON_COSMETIC_COUNT 17

#define CHARACTER_MAX_NAME_LENGTH 12

enum EquipSlot
{
    EQUIP_SLOT_ERROR                  = 0,
    EQUIP_SLOT_CAP                    = 1,
    EQUIP_SLOT_FOREHEAD               = 2, // To my knowledge, no item can be equipped here
    EQUIP_SLOT_EYE_ACCESSORY          = 3, // ditto
    EQUIP_SLOT_EAR_ACCESSORY          = 4, // ditto
    EQUIP_SLOT_TOP_OR_OVERALL         = 5,
    EQUIP_SLOT_BOTTOM                 = 6,
    EQUIP_SLOT_SHOES                  = 7,
    EQUIP_SLOT_GLOVES                 = 8,
    EQUIP_SLOT_MANTLE                 = 9,
    EQUIP_SLOT_SHIELD                 = 10,
    EQUIP_SLOT_WEAPON                 = 11,
    EQUIP_SLOT_PENDANT                = 17,
    EQUIP_SLOT_TAMING_MOB             = 18,
    EQUIP_SLOT_SADDLE                 = 19,
    EQUIP_SLOT_MOB_EQUIP              = 20, // Probably
    EQUIP_SLOT_MEDAL                  = 49,
    EQUIP_SLOT_BELT                   = 50,
    EQUIP_SLOT_CAP_COSMETIC           = 101,
    EQUIP_SLOT_FOREHEAD_COSMETIC      = 102,
    EQUIP_SLOT_EYE_ACCESSORY_COSMETIC = 103,
    EQUIP_SLOT_EAR_ACCESSORY_COSMETIC = 104,
    EQUIP_SLOT_CLOTHES_COSMETIC       = 105, // Also cosmetic overall
    EQUIP_SLOT_PANTS_COSMETIC         = 106,
    EQUIP_SLOT_SHOES_COSMETIC         = 107,
    EQUIP_SLOT_GLOVES_COSMETIC        = 108,
    EQUIP_SLOT_MANTLE_COSMETIC        = 109,
    EQUIP_SLOT_SHIELD_COSMETIC        = 110,
    EQUIP_SLOT_WEAPON_COSMETIC        = 111,
    EQUIP_SLOT_RING_1                 = 112, // Bottom left
    EQUIP_SLOT_RING_2                 = 113, // Bottom right
    EQUIP_SLOT_FIRST_PET_EQUIP        = 114,
    EQUIP_SLOT_RING_3                 = 115, // Top left
    EQUIP_SLOT_RING_4                 = 116, // Top right
    EQUIP_SLOT_PENDANT_COSMETIC       = 117, // Probably
    EQUIP_SLOT_TAMING_MOB_COSMETIC    = 118,
    EQUIP_SLOT_SADDLE_COSMETIC        = 119,
    EQUIP_SLOT_MOB_EQUIP_COSMETIC     = 120, // Probably
    EQUIP_SLOT_FIRST_PET_RING_1       = 121, // Left
    EQUIP_SLOT_FIRST_PET_ITEM_POC     = 122,
    EQUIP_SLOT_FIRST_PET_MESO_MAG     = 123,
    EQUIP_SLOT_HP_POC                 = 124,
    EQUIP_SLOT_MP_POC                 = 125,
    EQUIP_SLOT_FIRST_PET_POS          = 126,
    EQUIP_SLOT_FIRST_PET_MOVE_UP      = 127,
    EQUIP_SLOT_FIRST_PICK_UP          = 128,
    EQUIP_SLOT_FIRST_PET_RING_2       = 129, // Right
    EQUIP_SLOT_SECOND_PET_EQUIP       = 130, // Left
    EQUIP_SLOT_SECOND_PET_RING_1      = 131, // Left
    EQUIP_SLOT_SECOND_PET_RING_2      = 132, // Right
    EQUIP_SLOT_SECOND_PET_ITEM_POC    = 133,
    EQUIP_SLOT_SECOND_PET_MESO_MAG    = 134,
    EQUIP_SLOT_SECOND_PET_POS         = 135,
    EQUIP_SLOT_SECOND_PET_MOVE_UP     = 136,
    EQUIP_SLOT_SECOND_PET_PICK_UP     = 137,
    EQUIP_SLOT_THIRD_PET_EQUIP        = 138, // Left
    EQUIP_SLOT_THIRD_PET_RING_1       = 139, // Left
    EQUIP_SLOT_THIRD_PET_RING_2       = 140, // Right
    EQUIP_SLOT_THIRD_PET_ITEM_POC     = 141,
    EQUIP_SLOT_THIRD_PET_MESO_MAG     = 142,
    EQUIP_SLOT_THIRD_PET_POS          = 143,
    EQUIP_SLOT_THIRD_PET_MOVE_UP      = 144,
    EQUIP_SLOT_THIRD_PET_PICK_UP      = 145,
    EQUIP_SLOT_FIRST_PET_ITEM_IGNORE  = 146,
    EQUIP_SLOT_SECOND_PET_ITEM_IGNORE = 147,
    EQUIP_SLOT_THIRD_PET_ITEM_IGNORE  = 148,
};

static inline bool is_valid_equip_slot(enum EquipSlot slot)
{
    return (slot >= 1 && slot <= 11) || (slot >= 17 && slot <= 20) || slot == 49 || slot == 50 || (slot >= 101 && slot <= 148);
}

static inline enum EquipSlot equip_slot_from_compact(uint8_t compact)
{
    enum EquipSlot slot = compact + 1;

    if (compact >= 11)
        slot += 5;

    if (compact >= 15)
        slot += 28;

    if (compact >= 17)
        slot += 50;

    return slot;
}

static inline uint8_t equip_slot_to_compact(enum EquipSlot slot)
{
    uint8_t compact = slot - 1;

    if (slot >= 17)
        compact -= 5;

    if (slot >= 49)
        compact -= 28;

    if (slot >= 101)
        compact -= 50;

    return compact;
}

static inline enum EquipSlot equip_slot_from_id(uint32_t id)
{
    // TODO: Check if id is a cash item
    if (id / 10000 == 100)
        return EQUIP_SLOT_CAP;

    if (id / 10000 == 103)
        return EQUIP_SLOT_EAR_ACCESSORY;

    if (id / 10000 == 104 || id / 10000 == 105)
        return EQUIP_SLOT_TOP_OR_OVERALL;

    if (id / 10000 == 106)
        return EQUIP_SLOT_BOTTOM;

    if (id / 10000 == 107)
        return EQUIP_SLOT_SHOES;

    if (id / 10000 == 108)
        return EQUIP_SLOT_GLOVES;

    if (id / 10000 == 109)
        return EQUIP_SLOT_SHIELD;

    if (id / 10000 == 110)
        return EQUIP_SLOT_MANTLE;

    //if (id / 10000 == 111)
    //    return EQUIP_SLOT_RING;

    if (id / 10000 == 112)
        return EQUIP_SLOT_PENDANT;

    if (id / 10000 == 113)
        return EQUIP_SLOT_BELT;

    if (id / 10000 == 114)
        return EQUIP_SLOT_MEDAL;

    if (id / 100000 == 13 || id / 100000 == 14)
        return EQUIP_SLOT_WEAPON;

    return EQUIP_SLOT_ERROR;
}

enum EquipType {
    EQUIP_TYPE_ACCESSORY,
    EQUIP_TYPE_CAP,
    EQUIP_TYPE_CAPE,
    EQUIP_TYPE_COAT,
    EQUIP_TYPE_GLOVE,
    EQUIP_TYPE_LONGCOAT,
    EQUIP_TYPE_PANTS,
    EQUIP_TYPE_RING,
    EQUIP_TYPE_SHIELD,
    EQUIP_TYPE_SHOES,
    EQUIP_TYPE_WEAPON
};

static inline enum EquipType equip_type_from_id(uint32_t id)
{
    if (id / 10000 == 100)
        return EQUIP_TYPE_CAP;

    if (id / 10000 == 104)
        return EQUIP_TYPE_COAT;

    if (id / 10000 == 105)
        return EQUIP_TYPE_LONGCOAT;

    if (id / 10000 == 106)
        return EQUIP_TYPE_PANTS;

    //assert(false);
    return -1;
}

enum InventoryType {
    INVENTORY_TYPE_EQUIP,
    INVENTORY_TYPE_USE,
    INVENTORY_TYPE_SET_UP,
    INVENTORY_TYPE_ETC,
    INVENTORY_TYPE_CASH,
};

enum ItemFlags {
    //ITEM_FLAG_NONE          = 0x00000000,
    ITEM_FLAG_UNTRADABLE    = 0x00000001,
    ITEM_FLAG_ONE_OF_A_KIND = 0x00000002,
};

// Just the itemId and its equip slot, used for CharacterAppearance
struct ItemAppearance {
    enum EquipSlot equipSlot;
    uint32_t itemId;
};

struct Item {
    uint64_t id;
    uint32_t itemId;
    uint32_t cashId;
    uint32_t sn; // What is this?

    uint8_t ownerLength;
    char owner[CHARACTER_MAX_NAME_LENGTH];
    enum ItemFlags flags;
    int64_t expiration;
    uint8_t giftFromLength;
    char giftFrom[CHARACTER_MAX_NAME_LENGTH];
};

struct Equipment {
    uint64_t id;
    struct Item item;
    int8_t level;
    int8_t slots;
    int16_t str;
    int16_t dex;
    int16_t int_;
    int16_t luk;
    int16_t hp;
    int16_t mp;
    int16_t atk;
    int16_t matk;
    int16_t def;
    int16_t mdef;
    int16_t acc;
    int16_t avoid;
    int16_t hands;
    int16_t speed;
    int16_t jump;
    bool cash;
};

static inline struct Equipment equipment_from_info(const struct EquipInfo *info)
{
    return (struct Equipment) {
        .id = 0,
        .item = {
            .id = 0,
            .itemId = info->id,
            .flags = 0,
            .expiration = -1,
            .ownerLength = 0,
            .giftFromLength = 0
        },
        .level = 0,
        .str = info->str,
        .dex = info->dex,
        .int_ = info->int_,
        .luk = info->luk,
        .hp = info->hp,
        .mp = info->mp,
        .atk = info->atk,
        .matk = info->matk,
        .def = info->def,
        .mdef = info->mdef,
        .acc = info->acc,
        .avoid = info->avoid,
        .hands = 0,
        .speed = info->speed,
        .jump = info->jump,
        .slots = info->slots,
        .cash = info->cash
    };
}

struct InventoryItem {
    struct Item item;
    int16_t quantity;
};

// This is NOT an item that a pet uses; It is a pet as an item in a character's inventory
struct PetItem {
    struct Item item;
    unsigned int petId;
};

#define ITEM_IS_THROWING_STAR(id) ((id) / 10000 == 207)
#define ITEM_IS_BULLET(id) ((id) / 10000 == 233)
#define ITEM_IS_RECHARGEABLE(id) (ITEM_IS_THROWING_STAR(id) || ITEM_IS_BULLET(id))
#define ITEM_IS_CHAIR(id) ((id) / 10000 == 301)

static inline enum InventoryType item_get_inventory(uint32_t id)
{
    if (id / 1000000 == 1)
        return INVENTORY_TYPE_EQUIP;
    if (id / 1000000 == 2)
        return INVENTORY_TYPE_USE;
    if (id / 1000000 == 3)
        return INVENTORY_TYPE_SET_UP;
    if (id / 1000000 == 4)
        return INVENTORY_TYPE_ETC;

    return INVENTORY_TYPE_CASH;
}

#endif


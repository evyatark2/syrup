#include "character.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wz.h"

size_t quest_get_progress_string(struct Quest *quest, char *out)
{
    for (size_t i = 0; i < quest->progressCount; i++)
        snprintf(out + 3*i, 4, "%03" PRId32, quest->progress[i]);

    return quest->progressCount * 3;
}

struct CharacterStats character_to_character_stats(struct Character *chr)
{
    struct CharacterStats stats;
    stats.info.appearance = character_to_character_appearance(chr);

    stats.info.level = chr->level;
    stats.info.job = chr->job;
    stats.info.fame = chr->fame;

    stats.id = chr->id;
    stats.str = chr->str;
    stats.dex = chr->dex;
    stats.int_ = chr->int_;
    stats.luk = chr->luk;
    stats.maxHp = chr->maxHp;
    stats.hp = chr->hp;
    stats.maxMp = chr->maxMp;
    stats.mp = chr->mp;
    stats.ap = chr->ap;
    stats.sp = chr->sp;
    stats.exp = chr->exp;

    return stats;
}

struct CharacterAppearance character_to_character_appearance(struct Character *chr)
{
    struct CharacterAppearance appearance;
    appearance.nameLength = chr->nameLength;
    memcpy(appearance.name, chr->name, chr->nameLength);
    appearance.gender = chr->gender;
    appearance.skin = chr->skin;
    appearance.face = chr->face;
    appearance.hair = chr->hair;

    appearance.gachaExp = chr->gachaExp;
    appearance.map = chr->map;
    appearance.spawnPoint = chr->spawnPoint;

    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        appearance.equipments[i].isEmpty = chr->equippedEquipment[i].isEmpty;
        appearance.equipments[i].id = chr->equippedEquipment[i].equip.item.itemId;
    }

    return appearance;
}

int16_t character_get_effective_str(struct Character *chr)
{
    if (chr->str > INT16_MAX - chr->estr)
        return INT16_MAX;

    return chr->str + chr->estr;
}

int16_t character_get_effective_dex(struct Character *chr)
{
    if (chr->dex > INT16_MAX - chr->edex)
        return INT16_MAX;

    return chr->dex + chr->edex;
}

int16_t character_get_effective_int(struct Character *chr)
{
    if (chr->int_ > INT16_MAX - chr->eint)
        return INT16_MAX;

    return chr->int_ + chr->eint;
}

int16_t character_get_effective_luk(struct Character *chr)
{
    if (chr->luk > INT16_MAX - chr->eluk)
        return INT16_MAX;

    return chr->luk + chr->eluk;
}

int16_t character_get_effective_hp(struct Character *chr)
{
    if (chr->maxHp > HP_MAX - chr->eMaxHp)
        return HP_MAX;

    return chr->maxHp + chr->eMaxHp;
}

int16_t character_get_effective_mp(struct Character *chr)
{
    if (chr->maxMp > MP_MAX - chr->eMaxMp)
        return MP_MAX;

    return chr->maxMp + chr->eMaxMp;
}


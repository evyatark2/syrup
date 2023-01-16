#include <assert.h>
#include <string.h>

#include "writer.h"

void writer_init(struct Writer *writer, size_t size, uint8_t *packet)
{
    writer->size = size;
    writer->pos = 0;
    writer->packet = packet;
}

void writer_bool(struct Writer *writer, bool data)
{
    assert(writer->pos + 1 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 1);
    writer->pos += 1;
}

void writer_i8(struct Writer *writer, int8_t data)
{
    assert(writer->pos + 1 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 1);
    writer->pos += 1;
}

void writer_u8(struct Writer *writer, uint8_t data)
{
    assert(writer->pos + 1 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 1);
    writer->pos += 1;
}

void writer_i16(struct Writer *writer, int16_t data)
{
    assert(writer->pos + 2 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 2);
    writer->pos += 2;
}

void writer_u16(struct Writer *writer, uint16_t data)
{
    assert(writer->pos + 2 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 2);
    writer->pos += 2;
}

void writer_i32(struct Writer *writer, int32_t data)
{
    assert(writer->pos + 4 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 4);
    writer->pos += 4;
}

void writer_u32(struct Writer *writer, uint32_t data)
{
    assert(writer->pos + 4 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 4);
    writer->pos += 4;
}

void writer_i64(struct Writer *writer, int64_t data)
{
    assert(writer->pos + 8 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 8);
    writer->pos += 8;
}

void writer_u64(struct Writer *writer, uint64_t data)
{
    assert(writer->pos + 8 <= writer->size);

    memcpy(writer->packet + writer->pos, &data, 8);
    writer->pos += 8;

}

void writer_zero(struct Writer *writer, size_t count)
{
    memset(writer->packet + writer->pos, 0, count);
    writer->pos += count;
}

void writer_sized_string(struct Writer *writer, uint16_t size, const char *data)
{
    writer_u16(writer, size);
    assert(writer->pos + size <= writer->size);
    memcpy(writer->packet + writer->pos, data, size);
    writer->pos += size;
}

void writer_array(struct Writer *writer, uint16_t size, const uint8_t *data)
{
    assert(writer->pos + size <= writer->size);
    memcpy(writer->packet + writer->pos, data, size);
    writer->pos += size;
}

void writer_char_appearance(struct Writer *writer, struct CharacterAppearance *chr, bool mega)
{
    writer_u8(writer, chr->gender);
    writer_u8(writer, chr->skin);
    writer_u32(writer, chr->face);
    writer_u8(writer, mega ? 0 : 1);
    writer_u32(writer, chr->hair);
    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
        if (!chr->equipments[i].isEmpty) {
            writer_u8(writer, equip_slot_from_compact(i));
            writer_u32(writer, chr->equipments[i].id);
        }

        if (i == EQUIP_SLOT_NON_COSMETIC_COUNT)
            writer_u8(writer, 0xFF); // End of non-cosmetic equipment
    }

    // End of cosmetic equipment
    writer_u8(writer, 0xFF);

    // Cosmetic weapon
    if (!chr->equipments[equip_slot_to_compact(EQUIP_SLOT_WEAPON_COSMETIC)].isEmpty)
        writer_u32(writer, chr->equipments[equip_slot_to_compact(EQUIP_SLOT_WEAPON_COSMETIC)].id);
    else
        writer_u32(writer, 0);

    // Pets
    writer_u32(writer, 0);
    writer_u32(writer, 0);
    writer_u32(writer, 0);
}

void writer_char_stats(struct Writer *writer, struct CharacterStats *chr)
{
    writer_u32(writer, chr->id);
    writer_array(writer, chr->info.appearance.nameLength, chr->info.appearance.name);
    writer_zero(writer, 13 - chr->info.appearance.nameLength);
    writer_u8(writer, chr->info.appearance.gender);
    writer_u8(writer, chr->info.appearance.skin);
    writer_u32(writer, chr->info.appearance.face);
    writer_u32(writer, chr->info.appearance.hair);

    // Pets
    writer_u64(writer, 0);
    writer_u64(writer, 0);
    writer_u64(writer, 0);

    writer_u8(writer, chr->info.level);
    writer_u16(writer, chr->info.job);
    writer_i16(writer, chr->str);
    writer_i16(writer, chr->dex);
    writer_i16(writer, chr->int_);
    writer_i16(writer, chr->luk);
    writer_i16(writer, chr->hp);
    writer_i16(writer, chr->maxHp);
    writer_i16(writer, chr->mp);
    writer_i16(writer, chr->maxMp);
    writer_i16(writer, chr->ap);
    writer_i16(writer, chr->sp);
    writer_i32(writer, chr->exp);
    writer_i16(writer, chr->info.fame);
    writer_i32(writer, chr->info.appearance.gachaExp);
    writer_u32(writer, chr->info.appearance.map);
    writer_u8(writer, chr->info.appearance.spawnPoint); //
    writer_u32(writer, 0);
}


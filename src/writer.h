#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "character.h"

struct Writer {
    size_t size;
    size_t pos;
    uint8_t *packet;
};

void writer_init(struct Writer *writer, size_t size, uint8_t *packet);
void writer_bool(struct Writer *writer, bool data);
void writer_i8(struct Writer *writer, int8_t data);
void writer_u8(struct Writer *writer, uint8_t data);
void writer_i16(struct Writer *writer, int16_t data);
void writer_u16(struct Writer *writer, uint16_t data);
void writer_i32(struct Writer *writer, int32_t data);
void writer_u32(struct Writer *writer, uint32_t data);
void writer_i64(struct Writer *writer, int64_t data);
void writer_u64(struct Writer *writer, uint64_t data);
void writer_zero(struct Writer *writer, size_t count);
void writer_sized_string(struct Writer *writer, uint16_t size, const char *data);
void writer_array(struct Writer *writer, uint16_t size, const uint8_t *data);
#define WRITER_CHARACTER_APPEARANCE_MAX_LEN 349
void writer_char_appearance(struct Writer *writer, struct CharacterAppearance *chr, bool mega);
#define WRITER_CHARACTER_STATS_MAX_LEN 444
void writer_char_stats(struct Writer *writer, struct CharacterStats *chr);


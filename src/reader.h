#ifndef READER_H
#define READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct Reader {
    size_t size;
    size_t pos;
    uint8_t *packet;
};

void reader_init(struct Reader *reader, size_t size, uint8_t *packet);
bool reader_end(struct Reader *reader);
void reader_skip(struct Reader *reader, size_t size);
bool reader_i8(struct Reader *reader, int8_t *data);
bool reader_u8(struct Reader *reader, uint8_t *data);
bool reader_i16(struct Reader *reader, int16_t *data);
bool reader_u16(struct Reader *reader, uint16_t *data);
bool reader_i32(struct Reader *reader, int32_t *data);
bool reader_u32(struct Reader *reader, uint32_t *data);

/**
 * @param[in,out] max_len As input, it indicates the maximum length of \p string and as output it holds the number of bytes that were read
 */
bool reader_sized_string(struct Reader *reader, uint16_t *max_len, char *string);

bool reader_array(struct Reader *reader, size_t len, uint8_t *data);

#endif


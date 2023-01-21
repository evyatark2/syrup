#include "reader.h"

void reader_init(struct Reader *reader, size_t size, uint8_t *packet)
{
    reader->size = size;
    reader->pos = 0;
    reader->packet = packet;
}

bool reader_end(struct Reader *reader)
{
    return reader->pos == reader->size;
}

void reader_skip(struct Reader *reader, size_t len)
{
    reader->pos += len;
}

bool reader_i8(struct Reader *reader, int8_t *data)
{
    if (reader->pos + 1 > reader->size)
        return false;

    memcpy(data, reader->packet + reader->pos, 1);
    reader->pos += 1;
    return true;
}

bool reader_u8(struct Reader *reader, uint8_t *data)
{
    if (reader->pos + 1 > reader->size)
        return false;

    memcpy(data, reader->packet + reader->pos, 1);
    reader->pos += 1;
    return true;
}

bool reader_i16(struct Reader *reader, int16_t *data)
{
    if (reader->pos + 2 > reader->size)
        return false;

    memcpy(data, reader->packet + reader->pos, 2);
    reader->pos += 2;
    return true;
}

bool reader_u16(struct Reader *reader, uint16_t *data)
{
    if (reader->pos + 2 > reader->size)
        return false;

    memcpy(data, reader->packet + reader->pos, 2);
    reader->pos += 2;
    return true;
}

bool reader_i32(struct Reader *reader, int32_t *data)
{
    if (reader->pos + 4 > reader->size)
        return false;

    memcpy(data, reader->packet + reader->pos, 4);
    reader->pos += 4;
    return true;
}

bool reader_u32(struct Reader *reader, uint32_t *data)
{
    if (reader->pos + 4 > reader->size)
        return false;

    memcpy(data, reader->packet + reader->pos, 4);
    reader->pos += 4;
    return true;
}

/**
 * @param[in,out] max_len As input, it indicates the maximum length of \p string and as output, it holds the number of bytes that were written to \p string
 */
bool reader_sized_string(struct Reader *reader, uint16_t *max_len, char *string)
{
    uint16_t len;
    if (!reader_u16(reader, &len))
        return false;

    if (len > *max_len || reader->pos + len > reader->size)
        return false;

    *max_len = len;
    // TODO: Convert uint8_t to char
    memcpy(string, reader->packet + reader->pos, len);
    reader->pos += len;
    return true;
}

bool reader_array(struct Reader *reader, size_t len, uint8_t *data)
{
    if (reader->pos + len > reader->size)
        return false;

    memcpy(data, reader->packet + reader->pos, len);
    reader->pos += len;
    return true;
}


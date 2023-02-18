#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>

#define WORLD_COUNT 21
#define CHANNEL_COUNT 20

extern const int32_t EXP_TABLE[200];

#define DEFAULT_KEY_COUNT 40
extern const uint32_t DEFAULT_KEY[DEFAULT_KEY_COUNT];
extern const uint8_t DEFAULT_TYPE[DEFAULT_KEY_COUNT];
extern const uint32_t DEFAULT_ACTION[DEFAULT_KEY_COUNT];

#endif


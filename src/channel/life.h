#ifndef MONSTER_H
#define MONSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Monster {
    uint32_t oid;
    uint32_t id;
    int16_t x;
    int16_t y;
    uint16_t fh;
    uint8_t stance;
    int32_t hp;
};

struct Npc {
    uint32_t oid;
    uint32_t id;
    int16_t x;
    int16_t y;
    int16_t fh;
    int16_t cy;
    int16_t rx0;
    int16_t rx1;
    bool f;
};

#endif


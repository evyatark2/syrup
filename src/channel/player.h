#ifndef PLAYER_H
#define PLAYER_H

#include "../character.h"

struct Player {
    uint32_t id;
    uint8_t level;
    uint16_t job;
    uint8_t nameLength;
    char name[CHARACTER_MAX_NAME_LENGTH];
    struct CharacterAppearance appearance;
    uint32_t chair;
    int16_t x;
    int16_t y;
    uint8_t stance;
    uint16_t fh;
};

#endif


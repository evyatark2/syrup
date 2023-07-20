#ifndef CMD_H
#define CMD_H

#include <stdint.h>

enum SessionCommandType {
    SESSION_COMMAND_WARP,
    SESSION_COMMAND_ADD_VISIBLE_ITEM,
    SESSION_COMMAND_EFFECT,
};

struct SessionCommand {
    enum SessionCommandType type;
    union {
        struct {
            uint32_t map_id;
            uint8_t portal;
        } warp;
        struct {
            uint32_t item_id;
        } addVisibleItem;
        struct {
            uint32_t effect_id;
        } effect;
    };
};

#endif


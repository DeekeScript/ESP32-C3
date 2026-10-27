#pragma once

#include <stdint.h>

struct Cmd {
    uint16_t len;
    uint8_t data[180];
};

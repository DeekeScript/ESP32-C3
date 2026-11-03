#pragma once

#include <stdint.h>

struct Cmd {
    uint16_t len;
    uint16_t conn;
    uint8_t drop;
    uint8_t data[180];
};

struct Peer {
    bool used;
    uint16_t handle;
    int screenW;
    int screenH;
    int lastX;
    int lastY;
    bool finger;
    uint16_t rndLo;
    uint16_t rndHi;
    uint8_t mods;
    uint8_t keys[6];
    uint8_t lastRaw[160];
    uint16_t lastRawLen;
};

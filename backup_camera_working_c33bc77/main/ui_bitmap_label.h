#pragma once
#include <stdint.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    const uint16_t *pixels;
} ui_label_bitmap_t;

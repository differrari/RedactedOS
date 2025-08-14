#pragma once

#include "types.h"
#include "../graphic_types.h"
#include "../draw/draw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum HorizontalAlignment {
    Leading,
    HorizontalCenter,
    Trailing,
} HorizontalAlignment;

typedef enum VerticalAlignment {
    Top,
    Bottom,
    VerticalCenter,
} VerticalAlignment;  

typedef struct text_ui_config {
    const char* text;
    uint16_t font_size;
} text_ui_config;

typedef struct common_ui_config {
    gpu_point point;
    gpu_size size;
    HorizontalAlignment horizontal_align;
    VerticalAlignment vertical_align;
    color background_color;
    color foreground_color;
} common_ui_config;


void draw_label(uint32_t* ctx, text_ui_config text_config, common_ui_config common_config);
#ifdef __cplusplus
}
#endif
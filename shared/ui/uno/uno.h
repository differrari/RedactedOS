#pragma once

#include "types.h"
#include "../graphic_types.h"
#include "../draw/draw.h"

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

typedef struct rect_ui_config {
    uint32_t border_size;
    color border_color;
    uint32_t border_padding;
} rect_ui_config;

typedef struct common_ui_config {
    int_point point;
    gpu_size size;
    HorizontalAlignment horizontal_align;
    VerticalAlignment vertical_align;
    color background_color;
    color foreground_color;
} common_ui_config;

#ifdef __cplusplus
extern "C" {
#endif

#define DRAW(item, children)\
({\
common_ui_config parent = item;\
(void)parent;\
children;\
})

#define RELATIVE(a,b) { parent.point.x + a, parent.point.y + b }

common_ui_config label(draw_ctx *ctx, text_ui_config text_config, common_ui_config common_config);
common_ui_config textbox(draw_ctx *ctx, text_ui_config text_config, common_ui_config common_config);
common_ui_config rectangle(draw_ctx *ctx, rect_ui_config rect_config, common_ui_config common_config);

#ifdef __cplusplus
}
#endif
#pragma once

#include "types.h"
#include "ui/graphic_types.h"

typedef struct {
    char *system_name;
    int logo_repeat;
    int logo_div;
    gpu_point logo_asymmetry;
    int logo_padding;
    int logo_inner_x_const;
    int logo_outer_x_div;
    int logo_upper_y_div;
    int logo_lower_y_const;
    int logo_symbols_count;
    gpu_point *logo_points;
} boot_theme_t;

typedef struct {
    uint32_t bg_color;
    uint32_t cursor_color_deselected;
    uint32_t cursor_color_selected;
} system_theme_t;

typedef struct {
    char *panic_text;
    char *default_pwd;
} system_config_t;

bool load_theme();

extern boot_theme_t boot_theme;
extern system_theme_t system_theme;
extern system_config_t system_config;

#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0
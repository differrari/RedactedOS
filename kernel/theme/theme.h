#pragma once

#include "types.h"
#include "graphic_types.h"
#include "files/system_module.h"

typedef struct {
    int logo_repeat;
    int logo_screen_div;
    gpu_point logo_asymmetry;
    int logo_padding;
    int logo_inner_x_const;
    int logo_outer_x_div;
    int logo_upper_y_div;
    int logo_lower_y_const;
    int logo_points_count;
    int logo_steps;
    gpu_point *logo_points;
    bool play_startup_sound;
} boot_theme_t;

typedef struct {
    u32 bg_color;
    u32 accent_color;
    u32 err_color;
    u32 cursor_color_deselected;
    u32 cursor_color_selected;
    bool use_window_shadows;
} system_theme_t;

typedef struct {
    char *panic_text;
    char *default_pwd;
    char *system_name;
    char *app_directory;
    bool use_net;
} system_config_t;

extern boot_theme_t boot_theme;
extern system_theme_t system_theme;
extern system_config_t system_config;

extern system_module theme_mod;

#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0
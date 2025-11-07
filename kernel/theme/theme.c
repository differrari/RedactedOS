#include "theme.h"
#include "syscalls/syscalls.h"
#include "config/toml.h"
#include "default.h"
#include "console/kio.h"

gpu_point default_boot_offsets[BOOTSCREEN_NUM_SYMBOLS] = BOOTSCREEN_OFFSETS;
boot_theme_t boot_theme = {
    .logo_repeat = BOOTSCREEN_REPEAT,
    .logo_screen_div = BOOTSCREEN_DIV,
    .logo_asymmetry = BOOTSCREEN_ASYMM,
    .logo_padding = BOOTSCREEN_PADDING,
    .logo_inner_x_const = BOOTSCREEN_INNER_X_CONST,
    .logo_outer_x_div = BOOTSCREEN_OUTER_X_DIV,
    .logo_upper_y_div = BOOTSCREEN_UPPER_Y_DIV,
    .logo_lower_y_const = BOOTSCREEN_LOWER_Y_CONST,
    .logo_points_count = BOOTSCREEN_NUM_SYMBOLS,
    .logo_points = default_boot_offsets,
    .play_startup_sound = true,
};

system_theme_t system_theme = {
    .bg_color = BG_COLOR,
    .cursor_color_deselected = CURSOR_COLOR_DESELECTED,
    .cursor_color_selected = CURSOR_COLOR_SELECTED,
    .use_window_shadows = true,
};

system_config_t system_config = {
    .panic_text = PANIC_TEXT,
    .default_pwd = DEFAULT_PWD,
    .system_name = SYSTEM_NAME,
};

void parse_theme_kvp(const char *key, char *value, size_t value_len, void *context){
    if (strcmp("bg_color", key, true) == 0){
        system_theme.bg_color = parse_hex_u64(value, value_len);
    }
    if (strcmp("cursor_color_deselected", key, true) == 0){
        system_theme.cursor_color_deselected = parse_hex_u64(value, value_len);
    }
    if (strcmp("cursor_color_selected", key, true) == 0){
        system_theme.cursor_color_selected = parse_hex_u64(value, value_len);
    }
    if (strcmp("panic_text", key, true) == 0){
        system_config.panic_text = string_from_literal_length(value, value_len).data;
    }
    if (strcmp("system_name", key, true) == 0){
        system_config.system_name = string_from_literal_length(value, value_len).data;
    }
    if (strcmp("logo_points_count", key, true) == 0){
        boot_theme.logo_points_count = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_repeat", key, true) == 0){
        boot_theme.logo_repeat = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_repeat", key, true) == 0){
        boot_theme.logo_repeat = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_screen_div", key, true) == 0){
        boot_theme.logo_screen_div = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_padding", key, true) == 0){
        boot_theme.logo_padding = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_inner_x_const", key, true) == 0){
        boot_theme.logo_inner_x_const = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_outer_x_div", key, true) == 0){
        boot_theme.logo_outer_x_div = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_upper_y_div", key, true) == 0){
        boot_theme.logo_upper_y_div = parse_int_u64(value, value_len);
    }
    if (strcmp("logo_lower_y_const", key, true) == 0){
        boot_theme.logo_lower_y_const = parse_int_u64(value, value_len);
    }
    if (strcmp("use_window_shadows", key, true) == 0){
        system_theme.use_window_shadows = parse_int_u64(value, value_len);//TODO: boolean parsing
    }
    if (strcmp("play_startup_sound", key, true) == 0){
        boot_theme.play_startup_sound = parse_int_u64(value, value_len);//TODO: boolean parsing
    }
}

bool load_theme(){
    file fd = {};
    if (fopen("/boot/redos/theme.config", &fd) != FS_RESULT_SUCCESS) return false;
    kprintf("Opened file");
    char *buf = malloc(fd.size);
    if (fread(&fd, buf, fd.size) != fd.size) return false;
    kprintf("Read file");

    read_toml(buf, fd.size, parse_theme_kvp, 0);

    return true;
}
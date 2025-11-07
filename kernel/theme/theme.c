#include "theme.h"
#include "syscalls/syscalls.h"
#include "config/toml.h"
#include "default.h"

gpu_point default_boot_offsets[BOOTSCREEN_NUM_SYMBOLS] = BOOTSCREEN_OFFSETS;
boot_theme_t boot_theme = {
    .system_name = BOOTSCREEN_TEXT,
    .logo_repeat = BOOTSCREEN_REPEAT,
    .logo_div = BOOTSCREEN_DIV,
    .logo_asymmetry = BOOTSCREEN_ASYMM,
    .logo_padding = BOOTSCREEN_PADDING,
    .logo_inner_x_const = BOOTSCREEN_INNER_X_CONST,
    .logo_outer_x_div = BOOTSCREEN_OUTER_X_DIV,
    .logo_upper_y_div = BOOTSCREEN_UPPER_Y_DIV,
    .logo_lower_y_const = BOOTSCREEN_LOWER_Y_CONST,
    .logo_symbols_count = BOOTSCREEN_NUM_SYMBOLS,
    .logo_points = default_boot_offsets,
};

system_theme_t system_theme = {
    .bg_color = BG_COLOR,
    .cursor_color_deselected = CURSOR_COLOR_DESELECTED,
    .cursor_color_selected = CURSOR_COLOR_SELECTED,
};

system_config_t system_config = {
    .panic_text = PANIC_TEXT,
    .default_pwd = DEFAULT_PWD,
};

void parse_theme_kvp(const char *key, char *value, size_t value_len, void *context){

}

bool load_theme(){
    file fd = {};
    if (fopen("/boot/redos/theme.toml", &fd) != FS_RESULT_SUCCESS) return false;
    char *buf = malloc(fd.size);
    if (fread(&fd, buf, fd.size) != fd.size) return false;

    read_toml(buf, fd.size, parse_theme_kvp, 0);

    return true;
}
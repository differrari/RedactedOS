#include "theme.h"
#include "syscalls/syscalls.h"
#include "config/toml.h"
#include "default.h"
#include "console/kio.h"
#include "math/math.h"
#include "std/memory.h"

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
    .logo_steps = BOOTSCREEN_NUM_SYMBOLS-1,
    .logo_points = default_boot_offsets,
    .play_startup_sound = true,
};

system_theme_t system_theme = {
    .bg_color = BG_COLOR,
    .accent_color = COLOR_WHITE,
    .err_color = 0xFF000000,
    .cursor_color_deselected = CURSOR_COLOR_DESELECTED,
    .cursor_color_selected = CURSOR_COLOR_SELECTED,
    .use_window_shadows = true,
};

system_config_t system_config = {
    .panic_text = PANIC_TEXT,
    .default_pwd = DEFAULT_PWD,
    .system_name = SYSTEM_NAME,
    .app_directory = "shared",
    .use_net = true,
};

gpu_point parse_gpu_point(char *value, size_t value_len){
    const char *cursor = value;
    cursor = seek_to(cursor, ',');
    uint32_t x = parse_int64(value, cursor-value-1) & UINT32_MAX;
    uint32_t y = parse_int64(cursor, value_len-(cursor-value)) & UINT32_MAX;
    return (gpu_point){x,y};
}

gpu_point* parse_gpu_point_array(char *value, size_t value_len){
    int depth = 0;
    int count = 0;
    char *orig = value;
    char *last_comma = value;
    do {
        if (*value == '[') depth++;
        if (*value == ']') depth--;
        if (*value == ',' && depth == 0){ count++; last_comma = value + 1; }
        value++;
        value_len--;
    } while (*value && value_len);
    for (; last_comma < value; last_comma++){
        if (*last_comma > ' ' && *last_comma < '~'){
            count++;
            break;
        }
    }
    boot_theme.logo_points_count = count;
    gpu_point *points = malloc(count * sizeof(gpu_point));
    for (int i = 0; i < count; i++){
        char *start_index = 0;
        do {
            if (*orig == '[') start_index = orig + 1;
            if (*orig == ']' && start_index) { points[i] = parse_gpu_point(start_index, (orig-start_index)); break; } 
            orig++;
        } while (orig);
    }
    
    return points;
}

#define parse_toml(k,dest,func) if (strcmp_case(#k, key,true) == 0) dest.k = func(value,value_len)
#define parse_toml_str(k,dest) if (strcmp_case(#k, key,true) == 0) dest.k = string_from_literal_length(value,value_len).data

void parse_theme_kvp(const char *key, char *value, size_t value_len, void *context){
    parse_toml(bg_color,                system_theme, parse_hex_u64);
    parse_toml(accent_color,            system_theme, parse_hex_u64);
    parse_toml(err_color,               system_theme, parse_hex_u64);
    parse_toml(cursor_color_deselected, system_theme, parse_hex_u64);
    parse_toml(cursor_color_selected,   system_theme, parse_hex_u64);
    parse_toml(use_window_shadows,      system_theme,parse_int_u64);
    
    parse_toml_str(panic_text,  system_config);
    parse_toml_str(system_name, system_config);
    parse_toml_str(app_directory, system_config);
    parse_toml(use_net, system_config, parse_int_u64);
    
    parse_toml(logo_points_count,   boot_theme,parse_int_u64);
    parse_toml(logo_repeat,         boot_theme,parse_int_u64);
    parse_toml(logo_screen_div,     boot_theme,parse_int_u64);
    parse_toml(logo_padding,        boot_theme,parse_int_u64);
    parse_toml(logo_inner_x_const,  boot_theme,parse_int_u64);
    parse_toml(logo_outer_x_div,    boot_theme,parse_int_u64);
    parse_toml(logo_upper_y_div,    boot_theme,parse_int_u64);
    parse_toml(logo_lower_y_const,  boot_theme,parse_int_u64);
    parse_toml(play_startup_sound,  boot_theme,parse_int_u64);

    parse_toml(logo_asymmetry, boot_theme, parse_gpu_point);

    parse_toml(logo_steps, boot_theme, parse_int_u64);

    parse_toml(logo_points, boot_theme, parse_gpu_point_array);

}

bool load_theme(){
    file fd = {};
    if (openf("/boot/redos/theme.config", &fd) != FS_RESULT_SUCCESS) return false;
    char *buf = malloc(fd.size);
    if (readf(&fd, buf, fd.size) != fd.size) return false;
    closef(&fd);

    read_toml(buf, fd.size, parse_theme_kvp, 0);

    return true;
}

size_t read_theme(const char *path, void* buf, size_t size){
    size = min(size, sizeof(system_theme));
    kprintf("Accent color %x",system_theme.cursor_color_selected);
    memcpy(buf, (void*)&system_theme, size);
    return size;
}

system_module theme_mod = (system_module){
    .name = "theme",
    .mount = "/theme",
    .init = load_theme,
    .open = 0,
    .write = 0,
    .swrite = 0,
    .read = 0,
    .sread = read_theme,
};
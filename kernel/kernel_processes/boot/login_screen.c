#include "login_screen.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "../kprocess_loader.h"
#include "theme/theme.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "math/math.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "ui/uno/uno.h"

int login_screen(){
    sys_focus_current();
    sys_set_secure(true);
    char* buf = (char*)malloc(256);
    int len = 0;
    keypress old_kp;
    gpu_clear(system_theme.bg_color);
    while (1)
    {
        gpu_size screen_size = gpu_get_screen_size();
        gpu_point screen_middle = {screen_size.width/2,screen_size.height/2};
        string s = string_repeat('*',min(len,20));
        int scale = 2;
        uint32_t char_size = gpu_get_char_size(scale);
        int xo = screen_size.width / 3;
        int yo = screen_middle.y;
        int height = char_size * 2;

        draw_ctx *ctx = gpu_get_ctx();

        label(ctx, (text_ui_config){
            .text = system_config.system_name,
            .font_size = 2,
        }, (common_ui_config){
            .point = {0, yo - char_size*9},
            .size = { screen_size.width, yo },
            .horizontal_align = HorizontalCenter,
            .vertical_align = Top,
            .background_color = 0,
            .foreground_color = COLOR_WHITE,
        });

        label(ctx, (text_ui_config){
            .text = "Login",
            .font_size = 2,
        }, (common_ui_config){
            .point = {0, yo - char_size*6},
            .size = { screen_size.width, yo },
            .horizontal_align = HorizontalCenter,
            .vertical_align = Top,
            .background_color = 0,
            .foreground_color = COLOR_WHITE,
        });

        textbox(ctx, (text_ui_config){
            .text = s.data,
            .font_size = 2,
        }, (common_ui_config){
            .point = {xo, yo - char_size/2},
            .size = { screen_size.width/3, height },
            .horizontal_align = Leading,
            .vertical_align = VerticalCenter,
            .background_color = system_theme.bg_color+0x111111,
            .foreground_color = COLOR_WHITE,
        });

        keypress kp;
        if (sys_read_input_current(&kp)){
            for (int i = 0; i < 6; i++){
                char key = kp.keys[i];
                if (hid_keycode_to_char[(uint8_t)key]){
                    if (key == KEY_ENTER || key == KEY_KPENTER){
                        if (strcmp(buf,system_config.default_pwd) == 0){
                            free_sized(buf, 256);
                            free_sized(s.data,s.mem_length);
                            sys_set_secure(false);
                            stop_current_process(0);
                        } else
                            break;
                    }
                    key = hid_keycode_to_char[(uint8_t)key];
                    if (key != 0 && len < 256 && (!keypress_contains(&old_kp,kp.keys[i], kp.modifier) || !is_new_keypress(&old_kp, &kp))){
                        buf[len] = key;
                        len++;
                    }
                } 
                if (kp.keys[i] == KEY_BACKSPACE){
                    if (len > 0) len--;
                    buf[len] = '\0';
                } 
            }
        }

        old_kp = kp;
        gpu_flush();
        free_sized(s.data,s.mem_length);
    }
    return 1;
}

process_t* present_login(){
    return create_kernel_process("login",login_screen, 0, 0);
}
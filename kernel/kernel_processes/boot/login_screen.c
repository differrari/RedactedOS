#include "login_screen.h"
#include "memory/talloc.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "../kprocess_loader.h"
#include "theme/theme.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "math/math.h"
#include "std/string.h"
#include "syscalls/syscalls.h"

__attribute__((section(".text.kcoreprocesses")))
void login_screen(){
    sys_focus_current();
    sys_set_secure(true);
    char* buf = (char*)malloc(256);
    int len = 0;
    keypress old_kp;
    const char* name = BOOTSCREEN_TEXT;
    string title = string_l(name);
    string subtitle = string_l("Login");
    gpu_clear(BG_COLOR);
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
        
        gpu_draw_string(title, (gpu_point){screen_middle.x - ((title.length/2) * char_size), yo - char_size*9}, scale, 0xFFFFFFFF);
        gpu_draw_string(subtitle, (gpu_point){screen_middle.x - ((subtitle.length/2) * char_size), yo - char_size*6}, scale, 0xFFFFFFFF);

        gpu_fill_rect((gpu_rect){{xo,yo  - char_size/2}, {screen_size.width / 3, height}},BG_COLOR+0x111111);
        gpu_draw_string(s, (gpu_point){xo, yo}, scale, 0xFFFFFFFF);
        keypress kp;
        if (sys_read_input_current(&kp)){
            for (int i = 0; i < 6; i++){
                char key = kp.keys[i];
                if (hid_keycode_to_char[(uint8_t)key]){
                    if (key == KEY_ENTER || key == KEY_KEYPAD_ENTER){
                        if (strcmp(buf,default_pwd, false) == 0){
                            free(buf, 256);
                            free(s.data,s.mem_length);
                            free(title.data,title.mem_length);
                            free(subtitle.data,subtitle.mem_length);
                            sys_set_secure(false);
                            stop_current_process();
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
        free(s.data,s.mem_length);
    }
}

process_t* present_login(){
    return create_kernel_process("login",login_screen);
}
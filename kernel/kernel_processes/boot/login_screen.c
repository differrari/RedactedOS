#include "login_screen.h"
#include "ram_e.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "../kprocess_loader.h"
#include "theme/theme.h"
#include "process/scheduler.h"

__attribute__((section(".text.kcoreprocesses")))
void login_screen(){
    sys_focus_current();
    sys_set_secure(true);
    char* buf = (char*)talloc(256);
    int len = 0;
    while (1)
    {
        gpu_clear(BG_COLOR);
        size screen_size = gpu_get_screen_size();
        point screen_middle = {screen_size.width/2,screen_size.height/2};
        kstring s = string_l(buf);
        int scale = 2;
        uint32_t char_size = gpu_get_char_size(scale);
        int mid_offset = (s.length/2) * char_size;
        int xo = screen_middle.x - mid_offset;
        int yo = screen_middle.y;
        gpu_fill_rect((rect){xo,yo, char_size * s.length, char_size},BG_COLOR);
        gpu_draw_string(s, (point){xo, yo}, scale, 0xFFFFFF);
        keypress kp;
        if (sys_read_input_current(&kp))
            if (kp.keys[0] != 0 && len < 256){
                buf[len] = kp.keys[0];
                len++;
            }
        if (strcmp(buf,default_pwd) == 0)
            stop_current_process();
        gpu_flush();
        temp_free(s.data,s.length);
    }
}

process_t* present_login(){
    return create_kernel_process(login_screen);
}
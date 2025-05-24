#include "login_screen.h"
#include "memory/kalloc.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "../kprocess_loader.h"
#include "theme/theme.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "math/math.h"
#include "std/string.h"

static const char hid_keycode_to_char[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x2C] = ' ', [0x2D] = '-', [0x2E] = '=',
    [0x2F] = '[', [0x30] = ']', [0x31] = '\\', [0x33] = ';',
    [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.',
    [0x38] = '/',
};

bool keypress_contains(keypress *kp, char key, uint8_t modifier){
    if (kp->modifier != modifier) return false;//TODO: This is not entirely accurate, some modifiers do not change key

    for (int i = 0; i < 6; i++)
        if (kp->keys[i] == key)
            return true;
    return false;
}

__attribute__((section(".text.kcoreprocesses")))
void login_screen(){
    sys_focus_current();
    sys_set_secure(true);
    char* buf = (char*)talloc(256);
    int len = 0;
    keypress old_kp;
    const char* name = BOOTSCREEN_TEXT;
    kstring title = kstring_l(name);
    kstring subtitle = kstring_l("Login");
    while (1)
    {
        gpu_clear(BG_COLOR);
        gpu_size screen_size = gpu_get_screen_size();
        gpu_point screen_middle = {screen_size.width/2,screen_size.height/2};
        kstring s = kstring_repeat('*',min(len,20));
        int scale = 2;
        uint32_t char_size = gpu_get_char_size(scale);
        int xo = screen_size.width / 3;
        int yo = screen_middle.y;
        int height = char_size * 2;
        
        gpu_draw_string(title, (gpu_point){screen_middle.x - ((title.length/4) * char_size), yo - char_size*9}, scale, 0xFFFFFF);
        gpu_draw_string(subtitle, (gpu_point){screen_middle.x - ((subtitle.length/2) * char_size), yo - char_size*6}, scale, 0xFFFFFF);

        gpu_fill_rect((gpu_rect){xo,yo  - char_size/2, screen_size.width / 3, height},BG_COLOR+0x111111);
        gpu_draw_string(s, (gpu_point){xo, yo}, scale, 0xFFFFFF);
        keypress kp;
        if (sys_read_input_current(&kp)){
            for (int i = 0; i < 6; i++){
                char key = kp.keys[i];
                if (hid_keycode_to_char[key]){
                    if (key == KEY_ENTER){
                        if (strcmp(buf,default_pwd) == 0){
                            temp_free(s.data,s.length);
                            temp_free(title.data,title.length);
                            temp_free(subtitle.data,subtitle.length);
                            sys_set_secure(false);
                            stop_current_process();
                        } else
                            break;
                    }
                    key = hid_keycode_to_char[key];//Translate readables
                    if (key != 0 && len < 256 && !keypress_contains(&old_kp,kp.keys[i], kp.modifier) || identical_keypress(&old_kp, &kp)){
                        buf[len] = key;
                        kprintf("%c",key);
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
        temp_free(s.data,s.length);
    }
}

process_t* present_login(){
    return create_kernel_process("login",login_screen);
}
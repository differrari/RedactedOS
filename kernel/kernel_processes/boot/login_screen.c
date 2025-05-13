#include "login_screen.h"
#include "ram_e.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "../kprocess_loader.h"
#include "theme/theme.h"
#include "console/kio.h"
#include "process/scheduler.h"

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
        if (sys_read_input_current(&kp)){
            for (int i = 0; i < 6; i++){
                char key = kp.keys[i];
                if (hid_keycode_to_char[key]){
                    key = hid_keycode_to_char[key];//Translate readables
                    if (key != 0 && len < 256 && !keypress_contains(&old_kp,kp.keys[i], kp.modifier)){
                        buf[len] = key;
                        kprintf("%c",key);
                        len++;
                    }
                } 
                if (kp.keys[i] == 42){
                    if (len > 0) len--;
                    buf[len] = '\0';
                } 
            }
            kprintf(">");
        }
        if (strcmp(buf,default_pwd) == 0)
            stop_current_process();

        old_kp = kp;
        gpu_flush();
        temp_free(s.data,s.length);
    }
}

process_t* present_login(){
    return create_kernel_process(login_screen);
}
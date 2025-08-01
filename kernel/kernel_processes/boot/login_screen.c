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
#include "syscalls/syscalls.h"
#include "process/syscall.h"  // For SYS_POWEROFF
#include "power.h"  // For power_off()

//TODO: properly handle keypad
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
    [0x38] = '/', [0x58] = '\n',
};

bool keypress_contains(keypress *kp, char key, uint8_t modifier) {
    if (kp->modifier != modifier) return false; //TODO: This is not entirely accurate, some modifiers do not change key

    for (int i = 0; i < 6; i++)
        if (kp->keys[i] == key)
            return true;
    return false;
}

// Check if the keypress contains the Ctrl+Alt+Del combination
bool is_ctrl_alt_del(keypress *kp) {
    // Check for Ctrl+Alt modifiers (0x5 = 0x1 (CTRL) | 0x4 (ALT))
    if ((kp->modifier & (KEY_MOD_CTRL | KEY_MOD_ALT)) != (KEY_MOD_CTRL | KEY_MOD_ALT)) {
        return false;
    }

    // Check for Delete key in any of the key slots
    for (int i = 0; i < 6; i++) {
        if (kp->keys[i] == KEY_DELETE) {
            return true;
        }
    }
    return false;
}

// Check if the keypress contains the Ctrl+Alt+Backspace combination
bool is_ctrl_alt_backspace(keypress *kp) {
    // Check for Ctrl+Alt modifiers
    if ((kp->modifier & (KEY_MOD_CTRL | KEY_MOD_ALT)) != (KEY_MOD_CTRL | KEY_MOD_ALT)) {
        return false;
    }

    // Check for Backspace key in any of the key slots
    for (int i = 0; i < 6; i++) {
        if (kp->keys[i] == KEY_BACKSPACE) {
            return true;
        }
    }
    return false;
}

__attribute__((section(".text.kcoreprocesses")))
void login_screen(){
    sys_focus_current();
    sys_set_secure(true);
    char* buf = (char*)malloc(256);
    int len = 0;
    keypress old_kp;
    bool ctrl_alt_del_pressed = false;
    bool ctrl_alt_backspace_pressed = false;
    const char* name = BOOTSCREEN_TEXT;
    string title = string_l(name);
    string subtitle = string_l("Login");
    gpu_clear(BG_COLOR);
    // Shutdown hint position
    int button_padding = 10;

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
        
        // Draw title and subtitle
        gpu_draw_string(title, (gpu_point){screen_middle.x - ((title.length/2) * char_size), yo - char_size*9}, scale, 0xFFFFFFFF);
        gpu_draw_string(subtitle, (gpu_point){screen_middle.x - ((subtitle.length/2) * char_size), yo - char_size*6}, scale, 0xFFFFFFFF);

        // Draw password input field
        gpu_fill_rect((gpu_rect){{xo,yo  - char_size/2}, {screen_size.width / 3, height}},BG_COLOR+0x111111);
        gpu_draw_string(s, (gpu_point){xo, yo}, scale, 0xFFFFFFFF);

        // Draw shutdown hint
        string shutdown_hint = string_l("Press Ctrl+Alt+Del to shutdown");
        gpu_draw_string(shutdown_hint,
                       (gpu_point){screen_size.width - (shutdown_hint.length * char_size) - button_padding,
                                 screen_size.height - char_size - button_padding},
                       1, 0x888888); // Gray text

        keypress kp;
        if (sys_read_input_current(&kp)){
            // Check for Ctrl+Alt+Del to shutdown
            if (is_ctrl_alt_del(&kp)) {
                if (!ctrl_alt_del_pressed) {
                    ctrl_alt_del_pressed = true;
                    kprintf("\nShutting down system...\n");
                    power_off();
                    // If power_off() returns (which it shouldn't), just continue
                    continue;
                }
            } else {
                ctrl_alt_del_pressed = false;
            }

            // Check for Ctrl+Alt+Backspace to reboot
            if (is_ctrl_alt_backspace(&kp)) {
                if (!ctrl_alt_backspace_pressed) {
                    ctrl_alt_backspace_pressed = true;
                    kprintf("\nRebooting system...\n");
                    reboot();
                    // If reboot() returns (which it shouldn't), just continue
                    continue;
                }
            } else {
                ctrl_alt_backspace_pressed = false;
            }

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
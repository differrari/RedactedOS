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
#include "process/syscall.h"  // For SYS_POWEROFF
#include "power.h"  // For power_off()



// Check if the keypress contains the Ctrl+Alt+Del combination
bool is_ctrl_alt_del(keypress *kp) {
    // Check for any CTRL modifier (left or right)
    bool has_ctrl = (kp->modifier & (KEY_MOD_LCTRL | KEY_MOD_RCTRL)) != 0;
    // Check for any ALT modifier (left or right) 
    bool has_alt = (kp->modifier & (KEY_MOD_LALT | KEY_MOD_RALT)) != 0;
    
    if (!has_ctrl || !has_alt) {
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

// Check if the keypress contains the Ctrl+Alt+ESC combination
bool is_ctrl_alt_esc(keypress *kp) {
    // Check for any CTRL modifier (left or right)
    bool has_ctrl = (kp->modifier & (KEY_MOD_LCTRL | KEY_MOD_RCTRL)) != 0;
    // Check for any ALT modifier (left or right)
    bool has_alt = (kp->modifier & (KEY_MOD_LALT | KEY_MOD_RALT)) != 0;
    
    if (!has_ctrl || !has_alt) {
        return false;
    }

    // Check for ESC key in any of the key slots
    for (int i = 0; i < 6; i++) {
        if (kp->keys[i] == KEY_ESC) {
            return true;
        }
    }
    return false;
}

int login_screen(){
    sys_focus_current();
    sys_set_secure(true);
    char* buf = (char*)malloc(256);
    int len = 0;
    keypress old_kp;
    bool ctrl_alt_del_pressed = false;
    bool ctrl_alt_esc_pressed = false;
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

            // Check for Ctrl+Alt+ESC to reboot
            if (is_ctrl_alt_esc(&kp)) {
                if (!ctrl_alt_esc_pressed) {
                    ctrl_alt_esc_pressed = true;
                    kprintf("\nRebooting system...\n");
                    reboot();
                    // If reboot() returns (which it shouldn't), just continue
                    continue;
                }
            } else {
                ctrl_alt_esc_pressed = false;
            }

            for (int i = 0; i < 6; i++){
                char key = kp.keys[i];
                if (hid_keycode_to_char[(uint8_t)key]){
                    if (key == KEY_ENTER || key == KEY_KPENTER){
                        if (strcmp(buf,default_pwd, false) == 0){
                            free(buf, 256);
                            free(s.data,s.mem_length);
                            free(title.data,title.mem_length);
                            free(subtitle.data,subtitle.mem_length);
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
        free(s.data,s.mem_length);
    }
    return 1;
}

process_t* present_login(){
    return create_kernel_process("login",login_screen, 0, 0);
}
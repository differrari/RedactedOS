#include "kconsole.hpp"
#include "kconsole.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "terminal.hpp"

KernelConsole kconsole;

extern "C" void kconsole_putc(char c) {
    kconsole.put_char(c);
    gpu_flush();
}

extern "C" void kconsole_puts(const char *s) {
    kconsole.put_string(s);
}

extern "C" void kconsole_clear() {
    kconsole.clear();
}

extern "C" int toggle_visual(int argc, char* argv[]){
    keypress kp = {
        .modifier = KEY_MOD_LALT,
        .rsvd = 0,
        .keys = {0x13},
    };
    uint16_t shortcut = sys_subscribe_shortcut_current(kp);
    bool active = false;
    Terminal *terminal = new Terminal();
    terminal->initialize();
    while (1){
        if (sys_shortcut_triggered_current(shortcut)){
            active = !active;
            if (active){
                sys_focus_current();
                terminal->refresh();
            } else {
                terminal->clear();
            }
        }
        if (active)
            terminal->update();
    }
    return 1;
}

process_t* start_terminal(){
    return create_kernel_process("terminal",toggle_visual, 0, 0);
}
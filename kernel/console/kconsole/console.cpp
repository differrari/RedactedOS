#include "kconsole.hpp"
#include "kconsole.h"
#include "graph/graphics.h"
#include "input/input_dispatch.h"
#include "kernel_processes/windows/windows.h"

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

extern "C" void toggle_visual(){
    keypress kp = {
        .modifier = KEY_MOD_ALT,
        .rsvd = 0,
        .keys = {0x13},
    };
    uint16_t shortcut = sys_subscribe_shortcut_current(kp);
    bool active = false;
    KernelConsole *console = new KernelConsole();
    console->initialize();
    while (1){
        if (sys_shortcut_triggered_current(shortcut)){
            active = !active;
            if (active){
                pause_window_draw();
                console->refresh();
            } else {
                resume_window_draw();
                console->clear();
            }
        }
    }
}

process_t* start_terminal(){
    return create_kernel_process("terminal",toggle_visual);
}
#include "bootprocess.h"
#include "../kprocess_loader.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"
#include "theme/theme.h"
#include "input/input_dispatch.h"
#include "usb/usb.h"
#include "graph/graphics.h"
#include "kernel_processes/boot/screensaver.h"
#include "kernel_processes/boot/login_screen.h"
#include "kernel_processes/windows/dos.h"
#include "tools/tools.h"
#include "files/helpers.h"

enum {
    bsm_none,
    bsm_screensaver,
    bsm_login,
    bsm_services,
    bsm_userland
} boot_state;

process_t *current_proc;

void visual_init(){
    disable_visual();
    gpu_size screen_size = gpu_get_screen_size();
    mouse_config((gpu_point){(i32)screen_size.width/2,(i32)screen_size.height/2}, screen_size);
}

void load_service(const char *directory, const char *service){
    if (strlen(service) && *service == '.') return;
    string full_name = string_format("%s/%s",directory,service);
    process_t *proc = execute(full_name.data, 0, 0, EXEC_MODE_KEEP_FOCUS);
    string_free(full_name);
    if (!proc) print("[BOOT error] failed to load service %s",service);
}

void bootsm_transition(int new_state){
    boot_state = new_state;
    switch (boot_state) {
        case bsm_none: break;
        case bsm_screensaver: current_proc = start_screensaver(); break;
        case bsm_login: current_proc = present_login(); break;
        case bsm_services: {
            traverse_directory("/boot/redos/services", false, load_service);
            current_proc = 0;
            break;
        }
        case bsm_userland: {
            if (system_config.headless || !system_config.use_windows){
                const char *argv = system_config.headless ? "headless" : "";
                current_proc = execute("/boot/redos/system/terminal.red", 1, &argv, EXEC_MODE_DEFAULT);
            }
            else
                current_proc = create_windowing_system();
        } break;
    }
}

void bootsm_advance(){
    switch (boot_state) {
        case bsm_none: 
            if (system_config.use_login){
                bootsm_transition(bsm_login);
                break;
            }
            if (!system_config.headless){
                bootsm_transition(bsm_screensaver);
                break;
            }
            bootsm_transition(bsm_services); break;
        case bsm_screensaver: bootsm_transition(bsm_services); break;
        case bsm_login: bootsm_transition(system_config.headless ? bsm_services : bsm_screensaver); break;
        case bsm_services: bootsm_transition(bsm_userland); break;
        case bsm_userland: bootsm_transition(bsm_none); break;
    }
}

void eval_bootsm(){
    if (!current_proc || current_proc->state == STOPPED) bootsm_advance();
}

int eval_bootscreen(int argc, char* argv[]) {
    usb_start_polling();
    if (!system_config.headless) visual_init();
    bootsm_advance();
    while (1){
        eval_bootsm();
        msleep(25);
    }
    return 1;
}

void init_bootprocess() {
    create_kernel_process("bootsm",eval_bootscreen, 0, 0);
}

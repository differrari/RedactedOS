#include "launcher.h"
#include "desktop.hpp"
#include "kernel_processes/kprocess_loader.h"

extern "C" int manage_window(int argc, char* argv[]){
    Desktop *desktop = new Desktop();
    while (1)
    {
        desktop->draw_desktop();
    }
}

extern "C" process_t* launch_launcher(){
    return create_kernel_process("winmanager",manage_window, 0, 0);
}
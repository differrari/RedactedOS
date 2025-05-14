#include "windows.h"
#include "graph/graphics.h"
#include "../kprocess_loader.h"
#include "theme/theme.h"
#include "console/kio.h"
#include "window_manager.hpp"

WindowManager manager;

__attribute__((section(".text.kcoreprocesses")))
extern "C" void manage_windows(){
    while (1)
    {
        manager.manage_windows();
    }
}

extern "C" process_t* start_windows(){
    return create_kernel_process(manage_windows);
}
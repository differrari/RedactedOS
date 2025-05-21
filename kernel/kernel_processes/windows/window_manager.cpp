#include "window_manager.hpp"
#include "console/kio.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "../monitor/monitor_processes.h"
#include "std/std.hpp"

WindowManager::WindowManager(){
    
}

void WindowManager::initialize(){
    monitor_proc = start_process_monitor();
    desktop = new Desktop();
}

void WindowManager::manage_windows(){
    if (paused) return;
    desktop->draw_desktop();
}

//TODO: the window manager will need to handle gpu access from windows as well. Windows should work on their private FB and it should be written by the window manager into the real fb
//Tho it'd be nice to avoid the double repetition of writing to private, then backbuffer, then main buffer, but that's gpu-driver-specific
void WindowManager::pause(){
    paused = true;
}

void WindowManager::resume(){
    paused = false;
}
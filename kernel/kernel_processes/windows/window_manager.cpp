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

void WindowManager::pause(){
    paused = true;
}

void WindowManager::resume(){
    paused = false;
}
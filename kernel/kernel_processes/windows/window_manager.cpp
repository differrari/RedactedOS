#include "window_manager.hpp"
#include "console/kio.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "../monitor/monitor_processes.h"

WindowManager::WindowManager(){
    
}

void WindowManager::initialize(){
    monitor_proc = start_process_monitor();
    desktop = Desktop();
}

void WindowManager::manage_windows(){
    if (paused) return;
    desktop.draw_desktop();
}

void WindowManager::pause(){
    paused = true;
}

void WindowManager::resume(){
    paused = false;
}
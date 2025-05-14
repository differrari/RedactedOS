#include "window_manager.hpp"
#include "console/kio.h"
#include "graph/graphics.h"
#include "theme/theme.h"

WindowManager::WindowManager(){
    desktop = Desktop();
}

void WindowManager::manage_windows(){
    desktop.draw_desktop();
}
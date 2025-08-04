#pragma once

#include "types.h"
#include "desktop.hpp"
#include "process/process.h"

class WindowManager {
public:
    WindowManager();

    void manage_windows();
    void initialize();

    void pause();
    void resume();

private:
    Desktop *desktop;
    bool paused;
    process_t *monitor_proc;
};
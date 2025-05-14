#pragma once

#include "types.h"
#include "desktop.hpp"

class WindowManager {
public:
    WindowManager();

    void manage_windows();

    void pause();
    void resume();

private:
    Desktop desktop;
    bool paused;
};
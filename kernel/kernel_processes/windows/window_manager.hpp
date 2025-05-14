#pragma once

#include "types.h"
#include "desktop.hpp"

class WindowManager {
public:
    WindowManager();

    void manage_windows();

private:
    Desktop desktop;
};
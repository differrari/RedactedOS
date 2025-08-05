#pragma once

#include "types.h"
#include "graphic_types.h"

class UIElement {
public: 
    gpu_rect rect;
    void render();
};
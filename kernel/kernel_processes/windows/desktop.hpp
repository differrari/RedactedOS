#pragma once

#include "types.h"
#include "graph/graphics.h"

class Desktop {
public:
    Desktop();

    void draw_desktop();

private:
    gpu_size tile_size;
    gpu_point selected;
    void draw_tile(uint32_t column, uint32_t row);
    bool await_gpu();
    bool ready = false;
};
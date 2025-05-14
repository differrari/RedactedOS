#pragma once

#include "types.h"
#include "graph/graphics.h"

class Desktop {
public:
    Desktop();

    void draw_desktop();

private:
    size tile_size;
    point selected;
    void draw_tile(uint32_t column, uint32_t row);
    bool await_gpu();
    bool ready = false;
};
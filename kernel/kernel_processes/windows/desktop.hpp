#pragma once

#include "types.h"
#include "graph/graphics.h"
#include "process.h"
#include "ui/ui.hpp"
#include "std/std.hpp"

struct LaunchEntry {
    char* name;
    process_t* (*process_loader)();
};

class Desktop {
public:
    Desktop();

    void draw_desktop();

private:
    gpu_size tile_size;
    gpu_point selected;
    bool ready = false;
    bool renderedFull = false;
    process_t *active_proc;
    Label *single_label;// TODO: This is hardcoded, ew
    Array<LaunchEntry> entries;

    void draw_tile(uint32_t column, uint32_t row);
    bool await_gpu();
    void draw_full();
    void add_entry(char* name, process_t* (*process_loader)());
    void activate_current();
};
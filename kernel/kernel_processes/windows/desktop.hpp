#pragma once

#include "types.h"
#include "graph/graphics.h"
#include "process.h"

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
    void draw_tile(uint32_t column, uint32_t row);
    bool await_gpu();
    void draw_full();
    void add_entry(char* name, process_t* (*process_loader)());
    void activate_current();
    bool ready = false;
    bool renderedFull = false;
    LaunchEntry *entries;
    int num_entries;
    process_t *active_proc;
};
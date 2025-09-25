#pragma once

#include "types.h"
#include "graph/graphics.h"
#include "process/process.h"
#include "std/std.h"

struct LaunchEntry {
    char* name;
    char* ext;
    char* path;
};

class Launcher {
public:
    Launcher();

    void draw_desktop();

private:
    gpu_size tile_size;
    gpu_point selected;
    bool ready = false;
    bool rendered_full = false;
    process_t *active_proc;
    Array<LaunchEntry> entries;
    bool process_active = false;

    void load_entries();
    void draw_tile(uint32_t column, uint32_t row);
    bool await_gpu();
    void draw_full();
    void add_entry(char* name, char* ext, char* path);
    void activate_current();
    uint16_t find_extension(char *path);
    draw_ctx ctx;
};
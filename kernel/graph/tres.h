#pragma once

#include "types.h"
#include "ui/draw/draw.h"

#ifdef __cplusplus
extern "C" {
#endif
void init_window_manager(uintptr_t gpu_driver);

void create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

draw_ctx* get_window_ctx();

void commit_frame();

#ifdef __cplusplus
}
#endif
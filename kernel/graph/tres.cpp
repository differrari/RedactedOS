#include "tres.h"
#include "data_struct/linked_list.h"
#include "drivers/gpu_driver.hpp"

// clinkedlist_t windows;
typedef struct window_tab {
    gpu_point offset;
    draw_ctx win_ctx;
    uint16_t pid;
} window_tab;

window_tab test_window = {};

GPUDriver *main_gpu_driver;

void init_window_manager(uintptr_t gpu_driver){
    main_gpu_driver = (GPUDriver*)gpu_driver;
}

extern "C" void create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height){
    main_gpu_driver->create_window(x,y, width, height, &test_window.win_ctx);
}

void get_window_ctx(draw_ctx *ctx){
    *ctx = test_window.win_ctx;
}

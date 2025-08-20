#include "tres.h"
#include "data_struct/linked_list.h"
#include "drivers/gpu_driver.hpp"
#include "std/memfunctions.h"
#include "process/scheduler.h"

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
    test_window.offset.x = x;
    test_window.offset.y = y;
    main_gpu_driver->create_window(x,y, width, height, &test_window.win_ctx);
}

void get_window_ctx(draw_ctx* out_ctx){
    *out_ctx = test_window.win_ctx;
    test_window.pid = get_current_proc_pid();
}

void commit_frame(draw_ctx* frame_ctx){
    // if (x + width >= ctx->width || y + height >= ctx->height) return;
    draw_ctx win_ctx = test_window.win_ctx;
    draw_ctx *screen_ctx = main_gpu_driver->get_ctx();

    uint32_t sx = test_window.offset.x;
    uint32_t sy = test_window.offset.y;

    if (frame_ctx->full_redraw){
        for (uint32_t dy = 0; dy < win_ctx.height; dy++)
            memcpy(screen_ctx->fb + ((sy + dy) * screen_ctx->width) + sx, win_ctx.fb + (dy * win_ctx.width), win_ctx.width * 4);
        mark_dirty(screen_ctx, sx, sy, win_ctx.width, win_ctx.height);
    } else {
        for (uint32_t dr = 0; dr < frame_ctx->dirty_count; dr++){
            gpu_rect r = frame_ctx->dirty_rects[dr];
            for (uint32_t dy = 0; dy < r.size.height; dy++)
                memcpy(screen_ctx->fb + ((sy + dy + r.point.y) * screen_ctx->width) + sx + r.point.x, win_ctx.fb + (dy * win_ctx.width), r.size.width * 4);
            mark_dirty(screen_ctx, sx + r.point.x, sy + r.point.y, r.size.width, r.size.height);
        }
    }

    frame_ctx->dirty_count = 0;
    frame_ctx->full_redraw = false;
    
}
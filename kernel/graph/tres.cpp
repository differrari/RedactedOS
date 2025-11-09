#include "tres.h"
#include "drivers/gpu_driver.hpp"
#include "std/memory.h"
#include "process/scheduler.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscalls.h"
#include "kernel_processes/windows/launcher.h"
#include "console/kio.h"

clinkedlist_t *window_list;

uint16_t win_ids = 1;
bool dirty_windows = false;

typedef struct window_tab {
    gpu_point offset;
    draw_ctx win_ctx;
    uint16_t pid;
} window_tab;

GPUDriver *main_gpu_driver;

void init_window_manager(uintptr_t gpu_driver){
    window_list = clinkedlist_create();
    main_gpu_driver = (GPUDriver*)gpu_driver;
}

extern "C" void create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height){
    if (win_ids == UINT16_MAX) return;
    window_frame *frame = (window_frame*)malloc(sizeof(window_frame));
    frame->win_id = win_ids++;
    frame->width = width;
    frame->height = height;
    frame->x = x;
    frame->y = y;
    clinkedlist_push_front(window_list, frame);
    process_t *p = launch_launcher();
    p->win_id = frame->win_id;
    frame->pid = p->id;
    main_gpu_driver->create_window(x,y, width, height, &frame->win_ctx);
    dirty_windows = true;
}

int find_window(void *node, void *key){
    window_frame* frame = (window_frame*)node;
    uint16_t wid = *(uint16_t*)key;
    if (frame->win_id == wid) return 0;
    return -1;
}

void resize_window(uint32_t width, uint32_t height){
    process_t *p = get_current_proc();
    clinkedlist_node_t *node = clinkedlist_find(window_list, &p->win_id, find_window);
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        main_gpu_driver->resize_window(width, height, &frame->win_ctx);
        frame->width = width;
        frame->height = height;
        dirty_windows = true;
    }
}

void get_window_ctx(draw_ctx* out_ctx){
    process_t *p = get_current_proc();
    clinkedlist_node_t *node = clinkedlist_find(window_list, &p->win_id, find_window);
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        *out_ctx = frame->win_ctx;
        frame->pid = p->id;
    }
}

void commit_frame(draw_ctx* frame_ctx){
    process_t *p = get_current_proc();
    clinkedlist_node_t *node = clinkedlist_find(window_list, &p->win_id, find_window);
    if (!node || !node->data) return;
    window_frame* frame = (window_frame*)node->data;
    // if (x + width >= ctx->width || y + height >= ctx->height) return;
    draw_ctx win_ctx = frame->win_ctx;
    draw_ctx *screen_ctx = main_gpu_driver->get_ctx();

    uint32_t sx = frame->x;
    uint32_t sy = frame->y;

    if (frame_ctx->full_redraw){
        for (uint32_t dy = 0; dy < win_ctx.height; dy++)
            memcpy(screen_ctx->fb + ((sy + dy) * screen_ctx->width) + sx, frame_ctx->fb + (dy * win_ctx.width), win_ctx.width * 4);
        mark_dirty(screen_ctx, sx, sy, win_ctx.width, win_ctx.height);
    } else {
        for (uint32_t dr = 0; dr < frame_ctx->dirty_count; dr++){
            gpu_rect r = frame_ctx->dirty_rects[dr];
            for (uint32_t dy = 0; dy < r.size.height; dy++)
                memcpy(screen_ctx->fb + ((sy + dy + r.point.y) * screen_ctx->width) + sx + r.point.x, frame_ctx->fb + ((dy + r.point.y) * win_ctx.width) + r.point.x, r.size.width * 4);
            mark_dirty(screen_ctx, sx + r.point.x, sy + r.point.y, r.size.width, r.size.height);
        }
    }

    frame_ctx->dirty_count = 0;
    frame_ctx->full_redraw = false;
    
}
#pragma once

#include "types.h"
#include "ui/draw/draw.h"
#include "data_struct/linked_list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t win_id;
    uint32_t x, y;
    uint32_t width, height;
    draw_ctx win_ctx;
    uint16_t pid;
} window_frame;

void init_window_manager(uintptr_t gpu_driver);

void create_window(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

void resize_window(uint32_t width, uint32_t height);

void get_window_ctx(draw_ctx* out_ctx);

void commit_frame(draw_ctx* frame_ctx, window_frame* frame);

void set_window_focus(uint16_t win_id);
void unset_window_focus();

extern clinkedlist_t *window_list;

extern uint16_t win_ids;
extern bool dirty_windows;
extern gpu_point global_win_offset;

extern window_frame *focused_window;

#ifdef __cplusplus
}
#endif
#include "tres.h"
#include "std/memory.h"
#include "process/scheduler.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscalls.h"
#include "kernel_processes/windows/launcher.h"
#include "console/kio.h"
#include "sysregs.h"
#include "graphics.h"
#include "ui/graphic_types.h"

clinkedlist_t *window_list;
window_frame *focused_window;

uint16_t win_ids = 1;
bool dirty_windows = false;

int_point global_win_offset;

typedef struct window_tab {
    int_point offset;
    draw_ctx win_ctx;
    uint16_t pid;
} window_tab;

void init_window_manager(){
    window_list = clinkedlist_create();
}

int find_window(void *node, void *key){
    if (!node || !key) return -1;
    window_frame* frame = (window_frame*)node;
    uint16_t wid = *(uint16_t*)key;
    if (frame->win_id == wid) return 0;
    return -1;
}

gpu_point win_to_screen(window_frame *frame, gpu_point point){
    int_point frame_loc = (int_point){frame->x+global_win_offset.x, frame->y+global_win_offset.y};
    if ((int32_t)point.x > frame_loc.x && (int32_t)point.x < frame_loc.x + (int32_t)frame->width && (int32_t)point.y > frame_loc.y && (int32_t)point.y < frame_loc.y + (int32_t)frame->height)
        return (gpu_point){ point.x-frame->x, point.y-frame->y};
    return (gpu_point){};
}

gpu_point convert_mouse_position(gpu_point point){
    process_t *p = get_current_proc();
    clinkedlist_node_t *node = clinkedlist_find(window_list, PHYS_TO_VIRT_P(&p->win_id), (typeof(find_window)*)PHYS_TO_VIRT_P(find_window));
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        return win_to_screen(frame, point);
    }
    return (gpu_point){};
}

extern "C" void create_window(int32_t x, int32_t y, uint32_t width, uint32_t height){
    if (win_ids == UINT16_MAX) return;
    window_frame *frame = (window_frame*)malloc(sizeof(window_frame));
    frame->win_id = win_ids++;
    frame->width = width;
    frame->height = height;
    frame->x = x;
    frame->y = y;
    clinkedlist_push_front(window_list, PHYS_TO_VIRT_P(frame));
    gpu_create_window(x,y, width, height, &frame->win_ctx);
    process_t *p = launch_launcher();
    p->win_id = frame->win_id;
    frame->pid = p->id;
    dirty_windows = true;
}

void resize_window(uint32_t width, uint32_t height){
    process_t *p = get_current_proc();
    clinkedlist_node_t *node = clinkedlist_find(window_list, PHYS_TO_VIRT_P(&p->win_id), (typeof(find_window)*)PHYS_TO_VIRT_P(find_window));
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        gpu_resize_window(width, height, &frame->win_ctx);
        frame->width = width;
        frame->height = height;
        dirty_windows = true;
    }
}

void get_window_ctx(draw_ctx* out_ctx){
    process_t *p = get_current_proc();
    clinkedlist_node_t *node = clinkedlist_find(window_list, PHYS_TO_VIRT_P(&p->win_id), (typeof(find_window)*)PHYS_TO_VIRT_P(find_window));
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        if (out_ctx->width && out_ctx->height)
            resize_window(out_ctx->width, out_ctx->height);
        *out_ctx = frame->win_ctx;
        frame->pid = p->id;
    }
}

void commit_frame(draw_ctx* frame_ctx, window_frame* frame){
    if (!frame){
        process_t *p = get_current_proc();
        clinkedlist_node_t *node = clinkedlist_find(window_list, PHYS_TO_VIRT_P(&p->win_id), (typeof(find_window)*)PHYS_TO_VIRT_P(find_window));
        if (!node || !node->data) return;
        frame = (window_frame*)node->data;
    }

    draw_ctx win_ctx = frame->win_ctx;
    draw_ctx *screen_ctx = gpu_get_ctx();

    int32_t sx = global_win_offset.x + frame->x;
    int32_t sy = global_win_offset.y + frame->y;

    if (sx >= (int32_t)screen_ctx->width || sy >= (int32_t)screen_ctx->height || sx + win_ctx.width <= 0 || sy + win_ctx.height <= 0) return;

    int32_t w = win_ctx.width;
    int32_t h = win_ctx.height;

    uint32_t ox = 0;
    uint32_t oy = 0;
    
    if (sx < 0){
        w -= -sx;
        ox = -sx;
        sx = 0;
    }
    if (sy < 0){
        h -= -sy;
        oy = -sy;
        sy = 0;
    }

    if (sx + w > (int32_t)screen_ctx->width) w = screen_ctx->width - sx;
    else if (sx < 0){ w += sx; ox = -sx; sx = 0; }
    if (sy + h > (int32_t)screen_ctx->height) h = screen_ctx->height - sy;
    else if (sy < 0){ h += sy; oy = -sy; sy = 0; }
    if (w <= 0 || h <= 0) return;
    
    if (frame_ctx->full_redraw){
        for (int32_t dy = 0; dy < h; dy++)
            memcpy(screen_ctx->fb + ((sy + dy) * screen_ctx->width) + sx, win_ctx.fb + ((dy + oy) * win_ctx.width) + ox, w * sizeof(uint32_t));
        mark_dirty(screen_ctx, sx, sy, w, h);
    } else {
        for (uint32_t dr = 0; dr < frame_ctx->dirty_count; dr++){
            gpu_rect r = frame_ctx->dirty_rects[dr];
            for (uint32_t dy = 0; dy < r.size.height; dy++)
                memcpy(screen_ctx->fb + ((sy + dy + r.point.y) * screen_ctx->width) + sx + r.point.x, win_ctx.fb + ((dy + oy + r.point.y) * win_ctx.width) + r.point.x + ox, r.size.width * sizeof(uint32_t));
            mark_dirty(screen_ctx, sx + r.point.x, sy + r.point.y, r.size.width, r.size.height);
        }
    }

    frame_ctx->dirty_count = 0;
    frame_ctx->full_redraw = false;
    
}

void set_window_focus(uint16_t win_id){
    clinkedlist_node_t *node = clinkedlist_find(window_list, PHYS_TO_VIRT_P(&win_id), (typeof(find_window)*)PHYS_TO_VIRT_P(find_window));
    if (!node || !node->data) return;
    focused_window = (window_frame*)node->data;
    dirty_windows = true;
}

void unset_window_focus(){
    focused_window = 0;
}
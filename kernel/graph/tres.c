#include "tres.h"
#include "input/input_dispatch.h"
#include "process/scheduler.h"
#include "memory/memory.h"
#include "sysregs.h"
#include "graphics.h"
#include "graphic_types.h"
#include "tools/tools.h"
#include "math/math.h"
#include "memory/mm_process.h"
#include "memory/mmu.h"
#include "memory/addr.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "exceptions/irq.h"

linked_list_t *window_list;
window_frame *focused_window;

i32 zoom_scale = 1;

uint16_t win_ids = 1;
bool dirty_windows = false;

int_point global_win_offset;

typedef struct window_tab {
    int_point offset;
    draw_ctx win_ctx;
    uint16_t pid;
} window_tab;

void init_window_manager(){
    window_list = linked_list_create();
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
    linked_list_node_t *node = linked_list_find(window_list, PHYS_TO_VIRT_P(&p->win_id), PHYS_TO_VIRT_P(find_window));
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        return win_to_screen(frame, point);
    }
    return (gpu_point){};
}

i32 calculate_distance(i32 ep, i32 es, i32 np, i32 ns, i32 existing){
    i32 ef_min = ep;
    i32 ef_max = ep + es;
    
    i32 nf_min = np;
    i32 nf_max = np + ns;
    
    if (nf_max < ef_min || nf_min > ef_max) return 0;  
    
    i32 a = nf_max - ef_min + 10;
    i32 b = ef_max - nf_min + 10;
    
    if (existing) return existing > 0 ? a : b;
    
    return a < b ? a : -b;
}

int_point window_frame_intersect(window_frame *new_frame, window_frame *existing_frame, int_point existing_move){
    
    i32 horizontal = calculate_distance(existing_frame->x, existing_frame->width, new_frame->x, new_frame->width,existing_move.x);
    i32 vertical = calculate_distance(existing_frame->y, existing_frame->height, new_frame->y, new_frame->height,existing_move.y);
    return (int_point){ abs(horizontal) < abs(vertical) ? horizontal : 0, abs(horizontal) < abs(vertical) ? 0 : vertical };
}

void check_collisions(window_frame *frame){
    int_point move_dist = {};
    
    size_t num_wins = linked_list_count(window_list);
    for (size_t i = 0; i < num_wins; i++){
        linked_list_node_t *node = linked_list_get(window_list, i);
        if (!node || !node->data) continue;
        window_frame *ex_frame = node->data;
        if (!ex_frame || ex_frame == frame) continue;
        int_point new_move_dist = window_frame_intersect(ex_frame, frame, move_dist);
        if (new_move_dist.x){
            frame->x += new_move_dist.x;
            move_dist.x = new_move_dist.x;
            i = 0;
        }
        if (new_move_dist.y){
            frame->y += new_move_dist.y;
            move_dist.y = new_move_dist.y;
            i = 0;
        }
    }
}

bool create_window(i32 x, i32 y, u32 width, u32 height){
    if (win_ids == UINT16_MAX) return false;
    if (zoom_scale != 1) return false;
    if (width < 0x100 || height < 0x100) return false;
    
    window_frame *frame = (window_frame*)zalloc(sizeof(window_frame));
    frame->win_id = win_ids++;
    frame->width = width;
    frame->height = height;
    frame->x = x;
    frame->y = y;
    
    check_collisions(frame);
    
    draw_ctx *screen_ctx = gpu_get_ctx();
    int32_t sx = global_win_offset.x + frame->x;
    int32_t sy = global_win_offset.y + frame->y;

    if (sx >= (int32_t)screen_ctx->width || sy >= (int32_t)screen_ctx->height || sx + frame->width <= 0 || sy + frame->height <= 0) 
        global_win_offset = (int_point){-frame->x + 10,-frame->y + 10};
    
    linked_list_push_front(window_list, PHYS_TO_VIRT_P(frame));
    gpu_create_window(x,y, width, height, &frame->win_ctx);

    irq_flags_t irq = irq_save_disable();
    process_t *p = execute("/boot/redos/system/launcher.red/launcher.elf", 0, 0, 0);
    if (!p){
        irq_restore(irq);
        return false;
    }
    p->win_id = frame->win_id;
    frame->pid = p->id;
    sys_set_focus(p->id);
    dirty_windows = true;
    irq_restore(irq);

    return true;
}

void resize_window(uint32_t width, uint32_t height){
    process_t *p = get_current_proc();
    linked_list_node_t *node = linked_list_find(window_list, PHYS_TO_VIRT_P(&p->win_id), PHYS_TO_VIRT_P(find_window));
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        process_t *proc = get_all_processes();
        while (proc) {
            if (proc->win_id == frame->win_id && proc->mm.ttbr0 && proc->win_fb_va && proc->win_fb_size){
                mm_remove_vma(&proc->mm, proc->win_fb_va, proc->win_fb_va + proc->win_fb_size);
                for (uintptr_t va = proc->win_fb_va; va < proc->win_fb_va + proc->win_fb_size; va += PAGE_SIZE) mmu_unmap_and_get_pa((uint64_t*)proc->mm.ttbr0, va, 0);
                mmu_flush_asid(proc->mm.asid);
                proc->win_fb_va = 0;
                proc->win_fb_phys = 0;
                proc->win_fb_size = 0;
            }
            proc = proc->process_next;
        }
        gpu_resize_window(width, height, &frame->win_ctx);
        frame->width = width;
        frame->height = height;
        check_collisions(frame);
        dirty_windows = true;
    }
}

void get_window_ctx(draw_ctx* out_ctx){
    process_t *p = get_current_proc();
    linked_list_node_t *node = linked_list_find(window_list, PHYS_TO_VIRT_P(&p->win_id), PHYS_TO_VIRT_P(find_window));
    if (!node || !node->data) return;

    window_frame* frame = (window_frame*)node->data;
    if (out_ctx->width && out_ctx->height)
        resize_window(out_ctx->width, out_ctx->height);

    frame->pid = p->id;
    *out_ctx = frame->win_ctx;

    if (!p || !p->mm.ttbr0) return;

    size_t fb_size = (size_t)frame->win_ctx.width * (size_t)frame->win_ctx.height *sizeof(uint32_t);
    if (!fb_size) return;

    paddr_t pa = pt_va_to_pa(frame->win_ctx.fb);
    size_t map_size = count_pages(fb_size, PAGE_SIZE) * PAGE_SIZE;

    if (p->win_fb_va && (p->win_fb_size != map_size || p->win_fb_phys != pa)) {
        mm_remove_vma(&p->mm, p->win_fb_va, p->win_fb_va + p->win_fb_size);
        for (uintptr_t va = p->win_fb_va; va < p->win_fb_va + p->win_fb_size; va += PAGE_SIZE) mmu_unmap_and_get_pa((uint64_t*)p->mm.ttbr0, va, 0);
        mmu_flush_asid(p->mm.asid);
        p->win_fb_va = 0;
        p->win_fb_phys = 0;
        p->win_fb_size = 0;
    }

    if (!p->win_fb_va) {
        uintptr_t user_fb = mm_alloc_mmap(&p->mm, fb_size, MEM_RW, VMA_KIND_SPECIAL, 0);
        if (!user_fb) return;

        for (size_t off = 0; off < map_size; off += PAGE_SIZE) mmu_map_4kb((uint64_t*)p->mm.ttbr0, user_fb + off, pa + off, MAIR_IDX_NORMAL, MEM_RW | MEM_NORM, MEM_PRIV_USER);
        mmu_flush_asid(p->mm.asid);

        p->win_fb_va = user_fb;
        p->win_fb_phys = pa;
        p->win_fb_size = map_size;
    }

    out_ctx->fb = (uint32_t*)p->win_fb_va;
}

void commit_frame(draw_ctx* frame_ctx, window_frame* frame){
    if (!frame){
        process_t *p = get_current_proc();
        linked_list_node_t *node = linked_list_find(window_list, PHYS_TO_VIRT_P(&p->win_id), PHYS_TO_VIRT_P(find_window));
        if (!node || !node->data) return;
        frame = (window_frame*)node->data;
    }

    draw_ctx win_ctx = frame->win_ctx;
    draw_ctx *screen_ctx = gpu_get_ctx();

    int32_t sx = global_win_offset.x + frame->x;
    int32_t sy = global_win_offset.y + frame->y;
    
    sx /= zoom_scale;
    sy /= zoom_scale;

    if (sx >= (int32_t)screen_ctx->width || sy >= (int32_t)screen_ctx->height || sx + win_ctx.width <= 0 || sy + win_ctx.height <= 0) return;

    int32_t w = win_ctx.width;
    int32_t h = win_ctx.height;
    
    w /= zoom_scale;
    h /= zoom_scale;

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

    if (sx + w > (i32)screen_ctx->width) w = screen_ctx->width - sx;
    else if (sx < 0){ w += sx; ox = -sx; sx = 0; }
    if (sy + h > (i32)screen_ctx->height) h = screen_ctx->height - sy;
    else if (sy < 0){ h += sy; oy = -sy; sy = 0; }
    if (w <= 0 || h <= 0) return;
    
    if (zoom_scale != 1){
        for (i32 dy = 0; dy < h; dy++)
            for (i32 dx = 0; dx < w; dx++)
                screen_ctx->fb[((sy + dy) * screen_ctx->width) + (sx + dx)] = win_ctx.fb[(((dy * zoom_scale) + oy) * win_ctx.width) + ((dx * zoom_scale) + ox)];
    } else {
        if (frame_ctx->full_redraw){
            for (i32 dy = 0; dy < h; dy++)
                memcpy(screen_ctx->fb + ((sy + dy) * screen_ctx->width) + sx, win_ctx.fb + ((dy + oy) * win_ctx.width) + ox, w * sizeof(uint32_t));
            mark_dirty(screen_ctx, sx, sy, w, h);
        } else {
            for (uint32_t dr = 0; dr < frame_ctx->dirty_count; dr++){
                gpu_rect r = frame_ctx->dirty_rects[dr];
                for (u32 dy = 0; dy < r.size.height; dy++)
                    memcpy(screen_ctx->fb + ((sy + dy + r.point.y) * screen_ctx->width) + sx + r.point.x, win_ctx.fb + ((dy + oy + r.point.y) * win_ctx.width) + r.point.x + ox, r.size.width * sizeof(uint32_t));
                mark_dirty(screen_ctx, sx + r.point.x, sy + r.point.y, r.size.width, r.size.height);
            }
        }
    }

    frame_ctx->dirty_count = 0;
    frame_ctx->full_redraw = false;
    
}

u16 window_fallback_focus(u16 win_id, u16 skip_id){
    linked_list_node_t *node = linked_list_find(window_list, PHYS_TO_VIRT_P(&win_id), PHYS_TO_VIRT_P(find_window));
    if (!node || !node->data) return 0;

    process_t *proc = get_all_processes();
    while (proc) {
        if (proc->state != STOPPED && proc->win_id == win_id && proc->id != skip_id){
            window_frame *frame = node->data;
            frame->pid = proc->id;
            sys_set_focus(proc->id);
            return proc->id;
        }
        proc = proc->process_next;
    }

    release(node->data);
    linked_list_remove(window_list, node);
    dirty_windows = true;
    return 0;
}

void set_window_focus(uint16_t win_id){
    linked_list_node_t *node = linked_list_find(window_list, PHYS_TO_VIRT_P(&win_id), PHYS_TO_VIRT_P(find_window));
    if (!node || !node->data) return;
    focused_window = (window_frame*)node->data;
    dirty_windows = true;
}

void unset_window_focus(){
    focused_window = 0;
}
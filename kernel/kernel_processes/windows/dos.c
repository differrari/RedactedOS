#include "dos.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "ui/uno/uno.h"
#include "input/input_dispatch.h"
#include "console/kio.h"
#include "math/math.h"
#include "graph/tres.h"
#include "syscalls/syscalls.h"
#include "exceptions/irq.h"
#include "image/bmp.h"
#include "graph/tres.h"
#include "input_keycodes.h"
#include "wincomp.h"

#define BORDER_SIZE 3

static void draw_solid_window(draw_ctx *ctx, int_point fixed_point, gpu_size fixed_size, bool fill){
    DRAW(rectangle(ctx, (rect_ui_config){
        .border_size = BORDER_SIZE,
        .border_color = system_theme.bg_color + 0x222222
    }, (common_ui_config){
        .point = fixed_point,
        .size = fixed_size,
        .background_color = system_theme.bg_color,
        .foreground_color = COLOR_WHITE,
    }),{
        
    });
}

void draw_window(window_frame *frame){
    int_point fixed_point = { global_win_offset.x + frame->x - BORDER_SIZE, global_win_offset.y + frame->y - BORDER_SIZE };
    gpu_size fixed_size = { frame->width + BORDER_SIZE*2, frame->height + BORDER_SIZE*2 };
    draw_ctx *ctx = gpu_get_ctx();
    if (!system_theme.use_window_shadows || focused_window != frame){
        draw_solid_window(ctx, (int_point){(uint32_t)fixed_point.x,(uint32_t)fixed_point.y}, fixed_size, !frame->pid);
        return;
    }
    DRAW(rectangle(ctx, (rect_ui_config){
        .border_size = BORDER_SIZE * 1.5,
        .border_color = 0x44000000,
    }, (common_ui_config){ .point = (int_point){(uint32_t)fixed_point.x,(uint32_t)fixed_point.y}, .size = {fixed_size.width+BORDER_SIZE*1.5,fixed_size.height+BORDER_SIZE*1.5}, }),{
        draw_solid_window(ctx, (int_point){(uint32_t)fixed_point.x,(uint32_t)fixed_point.y}, fixed_size, !frame->pid);
    });
}

gpu_point click_loc;

static inline void calc_click(void *node){
    window_frame* frame = (window_frame*)node;
    gpu_point p = win_to_screen(frame, click_loc);
    if (!p.x || !p.y) return;
    sys_set_focus(frame->pid);
}

static inline void redraw_win(void *node){
    window_frame* frame = (window_frame*)node;
    draw_window(frame);
    frame->win_ctx.full_redraw = true; 
    commit_frame(&frame->win_ctx, frame);
}

bool use_desktop_img = false;
void *img;
image_info img_info;

static inline void draw_desktop(){
    if (img)
        fb_draw_img(gpu_get_ctx(), 0, 0, img, img_info.width, img_info.height);
    else
        gpu_clear(system_theme.bg_color);
}

uint32_t calc_average(uint32_t *color, size_t count){
    uint64_t pixr = 0;
    uint64_t pixg = 0;
    uint64_t pixb = 0;
    for (size_t i = 0; i < count; i++) {
        pixr += (color[i] >> 16) & 0xFF;
        pixg += (color[i] >> 8) & 0xFF;
        pixb += (color[i] >> 0) & 0xFF;
    }
    pixr /= count;
    pixg /= count;
    pixb /= count;
    return (0xFF << 24) | ((pixr & 0xFF) << 16) | ((pixg & 0xFF) << 8) | ((pixb & 0xFF) << 0);
}

uint32_t text_color_for_base(uint32_t base){
    uint8_t r = (base & 0xFF);
    uint8_t g = ((base << 8) & 0xFF);
    uint8_t b = ((base << 16) & 0xFF);
    uint8_t avg = (r+g+b)/3;
    if (avg < 0x77) avg = 255-avg;
    return (0xFF << 24) | (avg << 16) | (avg << 8) | avg; 
}

int window_system(){
    disable_visual();
    file fd = {};
    if (openf("/boot/redos/desktop.bmp", &fd) == FS_RESULT_SUCCESS){
        void *imgf = malloc(fd.size);
        readf(&fd, imgf, fd.size);
        image_info info = bmp_get_info(imgf, fd.size);
        img_info = (image_info){max(info.width,gpu_get_ctx()->width),max(info.height,gpu_get_ctx()->height)};
        bool need_resize = img_info.width != info.width || img_info.height != info.height;
        img = malloc(img_info.width * img_info.height * sizeof(uint32_t));
        void *oimg = need_resize ? malloc(info.width * info.height * sizeof(uint32_t)) : img;
        bmp_read_image(imgf, fd.size, oimg);
        if (need_resize){
            rescale_image(info.width, info.height, img_info.width, img_info.height, oimg, img);
            free_sized(oimg, info.width * info.height * sizeof(uint32_t));
        }
        system_theme.bg_color = calc_average(img, img_info.width * img_info.height);
        system_theme.accent_color = text_color_for_base(system_theme.bg_color);
        free_sized(imgf, fd.size);
        closef(&fd);
    }
    keypress kp_g = { 
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_G, 0, 0, 0, 0, 0}
    };
    uint16_t sid_g = sys_subscribe_shortcut_current(kp_g);
    keypress kp_f = { 
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_F, 0, 0, 0, 0, 0}
    };
    uint16_t sid_f = sys_subscribe_shortcut_current(kp_f);
    draw_desktop();
    
    uint16_t newwin_s = sys_subscribe_shortcut_current((keypress){
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_SPACE, 0, 0, 0, 0, 0}
    });
    
    uint16_t winl = sys_subscribe_shortcut_current((keypress){
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_LEFT, 0, 0, 0, 0, 0}
    });
    
    uint16_t winr = sys_subscribe_shortcut_current((keypress){
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_RIGHT, 0, 0, 0, 0, 0}
    });
    
    uint16_t winu = sys_subscribe_shortcut_current((keypress){
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_UP, 0, 0, 0, 0, 0}
    });
    
    uint16_t wind = sys_subscribe_shortcut_current((keypress){
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_DOWN, 0, 0, 0, 0, 0}
    });
    
    gpu_point start_point = {0,0};
    bool drawing = false;
    bool dragging = false;
    
    while (1){
        if (sys_shortcut_triggered_current(sid_g)){
            global_win_offset = (int_point){0,0};
            dirty_windows = true;
        }
        if (sys_shortcut_triggered_current(sid_f) && focused_window){
            global_win_offset = (int_point){-focused_window->x + BORDER_SIZE * 5,-focused_window->y + BORDER_SIZE * 5};
            dirty_windows = true;
        }
        if (sys_shortcut_triggered_current(newwin_s))
            new_managed_window();
        if (sys_shortcut_triggered_current(winl))
            switch_focus(1, 0);
        if (sys_shortcut_triggered_current(winr))
            switch_focus(-1, 0);
        if (sys_shortcut_triggered_current(winu))
            switch_focus(0, 1);
        if (sys_shortcut_triggered_current(wind))
            switch_focus(0, -1);
        if (mouse_button_pressed(MMB)){
            if (!dragging && !drawing){
                dragging = true;
                start_point = get_mouse_pos();
            }
            if (dragging){
                gpu_point end_point = get_mouse_pos();
                global_win_offset.x += end_point.x-start_point.x;
                global_win_offset.y += end_point.y-start_point.y;
                dirty_windows = true;
                start_point = end_point;
            }
        } 
        else dragging = false;
        if (mouse_button_pressed(LMB)){
            if (!drawing && !dragging){
                drawing = true;
                start_point = get_mouse_pos();
            }
        } else if (drawing){
            gpu_point end_point = get_mouse_pos();
            gpu_size size = {abs(end_point.x - start_point.x), abs(end_point.y - start_point.y)};
            if (size.width < 0x10 && size.height < 0x10){
                click_loc = end_point;
                clinkedlist_for_each(window_list, calc_click);
            } else {
                int_point fixed_point = { min(end_point.x,start_point.x),min(end_point.y,start_point.y) };
                disable_interrupt();
                create_window(fixed_point.x - global_win_offset.x,fixed_point.y - global_win_offset.y, size.width, size.height);
            }
            drawing = false;
        }
        disable_interrupt();
        if (dirty_windows){
            draw_desktop();
            clinkedlist_for_each(window_list, redraw_win);
            dirty_windows = false;
        }
        gpu_flush();
        enable_interrupt();
    }
    return 0;
}

process_t* create_windowing_system(){
    return create_kernel_process("dos", window_system, 0, 0);
}

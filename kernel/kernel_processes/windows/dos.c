#include "dos.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "ui/uno/uno.h"
#include "input/input_dispatch.h"
#include "console/kio.h"
#include "math/math.h"
#include "graph/tres.h"
#include "dev/driver_base.h"
#include "dev/module_loader.h"
#include "syscalls/syscalls.h"
#include "exceptions/irq.h"
#include "image/bmp.h"
#include "graph/tres.h"
#include "input_keycodes.h"

#define BORDER_SIZE 3

static void draw_solid_window(draw_ctx *ctx, gpu_point fixed_point, gpu_size fixed_size, bool fill){
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
    gpu_point fixed_point = { global_win_offset.x + frame->x - BORDER_SIZE, global_win_offset.y + frame->y - BORDER_SIZE };
    gpu_size fixed_size = { frame->width + BORDER_SIZE*2, frame->height + BORDER_SIZE*2 };
    draw_ctx *ctx = gpu_get_ctx();
    if (!system_theme.use_window_shadows || focused_window != frame){
        draw_solid_window(ctx, fixed_point, fixed_size, !frame->pid);
        return;
    }
    DRAW(rectangle(ctx, (rect_ui_config){
        .border_size = BORDER_SIZE * 1.5,
        .border_color = 0x44000000,
    }, (common_ui_config){ .point = fixed_point, .size = {fixed_size.width+BORDER_SIZE*1.5,fixed_size.height+BORDER_SIZE*1.5}, }),{
        draw_solid_window(ctx, fixed_point, fixed_size, !frame->pid);
    });
}

gpu_point click_loc;

static inline void calc_click(void *node){
    window_frame* frame = (window_frame*)node;
    if (click_loc.x < frame->x || click_loc.y < frame->y || click_loc.x > frame->x + frame->width || click_loc.y > frame->y + frame->height) return;
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

int window_system(){
    disable_visual();
    file fd = {};
    if (false && fopen("/boot/redos/desktop.bmp", &fd) == FS_RESULT_SUCCESS){
        void *imgf = malloc(fd.size);
        fread(&fd, imgf, fd.size);
        image_info info = bmp_get_info(imgf, fd.size);
        img_info = (image_info){max(info.width,gpu_get_ctx()->width),max(info.height,gpu_get_ctx()->height)};
        bool need_resize = img_info.width != info.width || img_info.height != info.height;
        img = malloc(img_info.width * img_info.height * sizeof(uint32_t));
        void *oimg = need_resize ? malloc(info.width * info.height * sizeof(uint32_t)) : img;
        bmp_read_image(imgf, fd.size, oimg);
        if (need_resize){
            rescale_image(info.width, info.height, img_info.width, img_info.height, oimg, img);
            free(oimg, info.width * info.height * sizeof(uint32_t));
        }
        free(imgf, fd.size);
        fclose(&fd);
    }
    keypress kp_g = { 
        .modifier = KEY_MOD_LCTRL,
        .keys = { KEY_G, 0, 0, 0, 0, 0}
    };
    uint16_t sid_g = sys_subscribe_shortcut_current(kp_g);
    keypress kp_f = { 
        .modifier = KEY_MOD_LCTRL,
        .keys = { KEY_F, 0, 0, 0, 0, 0}
    };
    uint16_t sid_f = sys_subscribe_shortcut_current(kp_f);
    draw_desktop();
    gpu_point start_point = {0,0};
    bool drawing = false;
    bool dragging = false;
    while (1){
        if (sys_shortcut_triggered_current(sid_g)){
            global_win_offset = (gpu_point){0,0};
            dirty_windows = true;
        }
        if (sys_shortcut_triggered_current(sid_f) && focused_window){
            global_win_offset = (gpu_point){-focused_window->x + BORDER_SIZE * 5,-focused_window->y + BORDER_SIZE * 5};
            dirty_windows = true;
        }
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
            }
            if (size.width < 0x100 || size.height < 0x100){
                drawing = false;
                continue;
            }
            gpu_point fixed_point = { min(end_point.x,start_point.x),min(end_point.y,start_point.y) };
            disable_interrupt();
            create_window(fixed_point.x - global_win_offset.x,fixed_point.y - global_win_offset.y, size.width, size.height);
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
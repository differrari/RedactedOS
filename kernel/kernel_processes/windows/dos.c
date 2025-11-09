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

#define BORDER_SIZE 3

static void draw_solid_window(draw_ctx *ctx, gpu_point fixed_point, gpu_size fixed_size){
    DRAW(rectangle(ctx, (rect_ui_config){
        .border_size = 3,
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
    if (!system_theme.use_window_shadows){
        draw_solid_window(ctx, fixed_point, fixed_size);
        return;
    }
    DRAW(rectangle(ctx, (rect_ui_config){}, (common_ui_config){
        .point = fixed_point,
        .size = {fixed_size.width+BORDER_SIZE*1.5,fixed_size.height+BORDER_SIZE*1.5},
        .background_color = 0x44000000,
        .foreground_color = 0,
    }),{
        draw_solid_window(ctx, fixed_point, fixed_size);
    });
}

static inline void redraw_win(void *node){
    window_frame* frame = (window_frame*)node;
    draw_window(frame);
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
    draw_desktop();
    gpu_point start_point = {0,0};
    bool drawing = false;
    bool dragging = false;
    while (1){
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
            if (size.width < 0x100 || size.height < 0x100){
                drawing = false;
                continue;
            }
            gpu_point fixed_point = { min(end_point.x,start_point.x),min(end_point.y,start_point.y) };
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
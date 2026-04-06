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
#include "image/png.h"
#include "graph/tres.h"
#include "input_keycodes.h"
#include "wincomp.h"

#define BORDER_SIZE 3

typedef enum { right_move, left_move, down_move, up_move } dos_movement;
u16 move_shortcuts[4];

typedef enum { window_mode, doodle_mode, mode_count } dos_mode;
u16 mode_shortcuts[mode_count];

u16 sid_g = 0;
u16 sid_f = 0;
u16 newwin_s = 0;

static dos_mode mode;
static draw_ctx *dos_ctx;

extern i32 zoom_scale;

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
    fixed_point.x /= zoom_scale;
    fixed_point.y /= zoom_scale;
    fixed_size.width /= zoom_scale;
    fixed_size.height /= zoom_scale;
    if (!system_theme.use_window_shadows || focused_window != frame){
        draw_solid_window(dos_ctx, (int_point){(uint32_t)fixed_point.x,(uint32_t)fixed_point.y}, fixed_size, !frame->pid);
        return;
    }
    DRAW(rectangle(dos_ctx, (rect_ui_config){
        .border_size = BORDER_SIZE * 1.5,
        .border_color = 0x44000000,
    }, (common_ui_config){ .point = (int_point){(uint32_t)fixed_point.x,(uint32_t)fixed_point.y}, .size = {fixed_size.width+BORDER_SIZE*1.5,fixed_size.height+BORDER_SIZE*1.5}, }),{
        draw_solid_window(dos_ctx, (int_point){(uint32_t)fixed_point.x,(uint32_t)fixed_point.y}, fixed_size, !frame->pid);
    });
}

gpu_point click_loc;
window_frame* clicked_frame;

static inline void calc_click(void *node){
    window_frame* frame = (window_frame*)node;
    gpu_point p = win_to_screen(frame, click_loc);
    if (!p.x || !p.y) return;
    clicked_frame = frame;
}

static inline void redraw_win(void *node){
    window_frame* frame = (window_frame*)node;
    draw_window(frame);
    frame->win_ctx.full_redraw = true; 
    commit_frame(&frame->win_ctx, frame);
}

bool use_desktop_img = false;
u32 *img;
draw_ctx img_draw_ctx;
image_info img_info;

static inline void draw_desktop(){
    draw_ctx *ctx = gpu_get_ctx();
    if (!ctx) return;
    if (img)
        fb_draw_img(dos_ctx, 0, 0, img, img_info.width, img_info.height);
    // else
    //     gpu_clear(system_theme.bg_color);
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

void setup_desktop_bg(){
    img_info = (image_info){ dos_ctx->width,dos_ctx->height };
    img = zalloc(img_info.width * img_info.height * sizeof(u32));
    img_draw_ctx = (draw_ctx){
        .width = img_info.width,
        .height = img_info.height,
        .stride = img_info.width * sizeof(u32),
        .fb = img
    };
    file fd = {};
    if (openf("/boot/redos/desktop.png", &fd) == FS_RESULT_SUCCESS){
        void *imgf = zalloc(fd.size);
        readf(&fd, imgf, fd.size);
        image_info info = png_get_info(imgf, fd.size);
        bool need_resize = img_info.width != info.width || img_info.height != info.height;
        void *oimg = need_resize ? zalloc(info.width * info.height * sizeof(uint32_t)) : img;
        png_read_image(imgf, fd.size, oimg);
        if (need_resize){
            rescale_image(info.width, info.height, img_info.width, img_info.height, oimg, img);
            release(oimg);
        }
        system_theme.bg_color = calc_average(img, img_info.width * img_info.height);
        system_theme.accent_color = text_color_for_base(system_theme.bg_color);
        release(imgf);
        closef(&fd);
    } else {
        fb_fill_rect(&img_draw_ctx, 0, 0, img_draw_ctx.width, img_draw_ctx.height, system_theme.bg_color);
    }
}

void setup_shortcuts(){
    sid_f = sys_subscribe_shortcut_current((keypress){ 
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_F, 0, 0, 0, 0, 0}
    });
    sid_g = sys_subscribe_shortcut_current((keypress){ 
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_G, 0, 0, 0, 0, 0}
    });
    newwin_s = sys_subscribe_shortcut_current((keypress){
        .modifier = KEY_MOD_LMETA,
        .keys = { KEY_ENTER, 0, 0, 0, 0, 0}
    });
    
    for (int i = 0; i < 4; i++)
        move_shortcuts[i] = sys_subscribe_shortcut_current((keypress){
            .modifier = KEY_MOD_LMETA,
            .keys = { KEY_RIGHT + i, 0, 0, 0, 0, 0}
        });
    
    for (int i = 0; i < mode_count; i++)
        mode_shortcuts[i] = sys_subscribe_shortcut_current((keypress){
            .modifier = KEY_MOD_LALT,
            .keys = { KEY_1 + i }
        });
}

void check_shortcuts(){
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
    for (int i = 0; i < 4; i++)
        if (sys_shortcut_triggered_current(move_shortcuts[i])){
            int sign = i % 2 == 0 ? -1 : 1;
            switch_focus(i < 2 ? sign : 0, i >= 2 ? sign : 0);   
        }
    for (int i = 0; i < mode_count; i++)
        if (sys_shortcut_triggered_current(mode_shortcuts[i])) mode = i;
}

int window_system(){
    disable_visual();
    dos_ctx = gpu_get_ctx();
    setup_desktop_bg();
    draw_desktop();
    setup_shortcuts();
    
    gpu_point start_point = {0,0};
    bool drawing = false;
    bool dragging = false;
    
    while (1){
        bool active = false;
        check_shortcuts();
        if (mouse_button_pressed(MMB)){
            active = true;
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
            active = true;
            switch (mode){
                case doodle_mode: {
                    gpu_point p = get_mouse_pos();
                    if (p.x != start_point.x || p.y != start_point.y){
                        fb_draw_line(&img_draw_ctx, start_point.x, start_point.y, p.x, p.y, 0xFFFFFFFF);
                        start_point = p;
                        dirty_windows = true;
                    }
                } break;
                default: break;
            }
        } else if (drawing){
            switch (mode){
                case window_mode: {
                    gpu_point end_point = get_mouse_pos();
                    gpu_size size = {abs(end_point.x - start_point.x), abs(end_point.y - start_point.y)};
                    click_loc = end_point;
                    if (size.width < 0x10 && size.height < 0x10){
                        clicked_frame = 0;
                        linked_list_for_each(window_list, calc_click);
                        if (clicked_frame) sys_set_focus(clicked_frame->pid);
                    } else {
                        int_point fixed_point = { min(end_point.x,start_point.x),min(end_point.y,start_point.y) };
                        disable_interrupt();
                        create_window(fixed_point.x - global_win_offset.x,fixed_point.y - global_win_offset.y, size.width, size.height);
                    }
                } break;
                default: break;
            }
            drawing = false;
        }
        // i8 scroll = get_raw_mouse_in().scroll;
        // if (scroll){
        //     zoom_scale += -scroll;
        //     zoom_scale = clamp(zoom_scale, 1, 5);
        //     dirty_windows = true;
        // }
        disable_interrupt();
        if (dirty_windows){
            active = true;
            draw_desktop();
            linked_list_for_each(window_list, redraw_win);
            dirty_windows = false;
        }
        gpu_flush();
        enable_interrupt();
        if (!active && !dirty_windows && !mouse_button_pressed(LMB) && !mouse_button_pressed(MMB)) msleep(25);
    }
    return 0;
}

process_t* create_windowing_system(){
    return create_kernel_process("dos", window_system, 0, 0);
}

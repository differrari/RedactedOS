#include "menu.h"

#include "graphic_types.h"
#include "graph/graphics.h"
#include "graph/tres.h"
#include "theme/theme.h"
#include "syscalls/syscalls.h"
#include "input/input_dispatch.h"
#include "filesystem/modules/fs_isolation.h"
#include "filesystem/filesystem.h"
#include "data/struct/stack.h"
#include "math/math.h"
#include "memory/memory.h"
#include "menu/load.h"

#define draw_eye(x_off, blink, eyelid) fb_fill_rect(ctx, rect.point.x + eye_margin + x_off, rect.point.y + (rect.size.height * 0.1), eye_size.width, eye_size.height, (blink) ? eyelid : 0xFFcccccc);\
fb_fill_rect(ctx, rect.point.x + eye_margin + p.x + x_off, rect.point.y + (rect.size.height * 0.1) + p.y, eye_size.width-pupil_distance.width, eye_size.height-pupil_distance.height, (blink) ? eyelid : 0xFF333333);

void test_widget(draw_ctx *ctx, gpu_rect rect, color bg){

    gpu_size eye_size = {rect.size.width * 0.2,rect.size.height * 0.3};
    i32 eye_distance = eye_size.width * 1.1;
    i32 eye_margin = (rect.size.width - (eye_size.width*2 + eye_distance))/2;

    gpu_size pupil_distance = {eye_size.width*0.3,eye_size.height*0.3};
    
    mouse_data data;
    get_mouse_status(&data);

    gpu_point p = get_mouse_pos();
    p.x = p.x * pupil_distance.width/ctx->width;
    p.y = p.y * pupil_distance.height/ctx->height;

    draw_eye(0,data.raw.buttons & 1, bg+0x111111);
    draw_eye(eye_size.width + eye_distance,(data.raw.buttons >> 1) & 1, bg+0x111111);

    fb_fill_rect(ctx, rect.point.x + eye_margin, rect.size.height - eye_size.height - eye_size.height/3, rect.size.width-(eye_margin*2), eye_size.height-(get_raw_mouse_in().scroll*3), 0xFF222222);
    if ((data.raw.buttons >> 2) & 1) fb_fill_rect(ctx, rect.point.x + eye_margin, rect.size.height - eye_size.height/2 - eye_size.height/3, rect.size.width-(eye_margin*2), eye_size.height/2, 0xFF442222);
    
}

void refresh_menu(){
    set_menu_color(system_theme.palette.background);
    menu_dirty = true;  
}

void load_menu(){
    menu_reset();
    load_menu_level(0);
}

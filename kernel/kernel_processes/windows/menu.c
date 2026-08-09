#include "menu.h"

#include "graphic_types.h"
#include "graph/graphics.h"
#include "graph/tres.h"
#include "theme/theme.h"
#include "syscalls/syscalls.h"
#include "input/input_dispatch.h"
#include "filesystem/modules/fs_isolation.h"
#include "filesystem/filesystem.h"

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

bool menu_dirty = true;

void refresh_menu(){
#if false
    menu_dirty = true;  
#endif
}

void load_menu(){
    process_t *menu_proc = get_proc_by_pid(sys_get_focused_pid());
    if (!menu_proc){ print("[MENU debug] no focused process"); return; }
    const char *path = "/menu";
    module_root *localfs = get_fs_for_id(menu_proc->permissions.fs_id);

    if (!localfs) return;

    void *buf = zalloc(0x1000);
    u64 off = 0;
    size_t s = list_directory_contents(localfs, path, buf, 0x1000, &off);
    print("Size of read %x",s);

    print("There are %i entries",*(u32*)buf);
}

void draw_menu(){
    if (menu_dirty){
        load_menu();
        menu_dirty = false;
    }
    draw_ctx *screen_ctx = gpu_get_ctx();
    fb_fill_rect(screen_ctx, 0, 0, screen_ctx->width, MENU_HEIGHT, system_theme.bg_color+0x111111);
    fb_fill_rect(screen_ctx, 0, MENU_HEIGHT-BORDER_SIZE, screen_ctx->width, BORDER_SIZE, 0x44000000);

    // fb_fill_rect(screen_ctx, screen_ctx->width/2 - 100, 0, screen_ctx->width/2 + 100, MENU_HEIGHT, 0xb4dd13);

    test_widget(screen_ctx, (gpu_rect){{screen_ctx->width/2 - 100, 0}, {200, MENU_HEIGHT}},system_theme.bg_color+0x111111);
}
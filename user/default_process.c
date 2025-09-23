#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/bmp.h"

#define BORDER 20

int proc_func() {
    draw_ctx ctx = {};
    request_draw_ctx(&ctx);
    image_info info;
    void *img = load_bmp("/boot/redos/images/jest.bmp", &info);
    resize_draw_ctx(&ctx, info.width+BORDER*2, info.height+BORDER*2);
    while (1) {
        mouse_input mouse = {};
        get_mouse_status(&mouse);
        // printf("Mouse %i",mouse.x);
        keypress kp = {};
        // printf("Print console test %f", (get_time()/1000.f));
        if (read_key(&kp))
            if (kp.keys[0] == KEY_ESC)
                halt(0);
        fb_clear(&ctx, 0xFFFFFFFF);
        // fb_fill_rect(&ctx, rect.point.x, rect.point.y, rect.size.width, rect.size.height, 0xFF222233);
        if (img) fb_draw_img(&ctx, BORDER, BORDER, img, info.width, info.height);
        // fb_draw_string(&ctx, "Print screen test", rect.point.x, rect.point.y, 2, 0xFFFFFFFF);
        commit_draw_ctx(&ctx);
    }
    return 1;
}
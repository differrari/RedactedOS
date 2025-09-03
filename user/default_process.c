#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"

void proc_func() {
    draw_ctx ctx = {};
    request_draw_ctx(&ctx);
    gpu_size size = (gpu_size){ctx.width,ctx.height};
    gpu_rect rect = (gpu_rect){{10,10},{size.width-20,size.height-20}};
    while (1) {
        keypress kp = {};
        printf("Print console test %f", (get_time()/1000.f));
        if (read_key(&kp))
            if (kp.keys[0] == KEY_ESC)
                halt(0);
        fb_clear(&ctx, 0xFFFFFFFF);
        fb_fill_rect(&ctx, rect.point.x, rect.point.y, rect.size.width, rect.size.height, 0xFF222233);
        fb_draw_string(&ctx, "Print screen test", rect.point.x, rect.point.y, 2, 0xFFFFFFFF);
        commit_draw_ctx(&ctx);
    }
    halt(1);
}
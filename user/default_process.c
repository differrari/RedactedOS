#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/bmp.h"

int proc_func() {
    draw_ctx ctx = {};
    request_draw_ctx(&ctx);
    gpu_size size = (gpu_size){ctx.width,ctx.height};
    gpu_rect rect = (gpu_rect){{10,10},{size.width-20,size.height-20}};
    file descriptor = {};
    FS_RESULT res = fopen("/boot/redos/images/bmp_24.bmp", &descriptor);
    void *img;
    image_info img_info;
    if (res == FS_RESULT_SUCCESS){
        void *img_file = (void*)malloc(descriptor.size);
        fread(&descriptor, img_file, descriptor.size);
        img_info = bmp_get_info(img_file, descriptor.size);
        if (img_info.width > 0 && img_info.height > 0){
            size_t img_size = img_info.width * img_info.height * system_bpp;
            img = (void*)malloc(img_size);
            bmp_read_image(img_file, descriptor.size, img);
        } else { 
            printf("Wrong image size %i",img_info.width,img_info.height);
            return -1;
        }
    } else { 
        printf("Failed to open image");
        return -1;
    }
    while (1) {
        mouse_input mouse = {};
        get_mouse_status(&mouse);
        printf("Mouse %i",mouse.x);
        keypress kp = {};
        printf("Print console test %f", (get_time()/1000.f));
        if (read_key(&kp))
            if (kp.keys[0] == KEY_ESC)
                halt(0);
        fb_clear(&ctx, 0xFFFFFFFF);
        printf("%xx%x",img_info.width,img_info.height);
        // fb_fill_rect(&ctx, rect.point.x, rect.point.y, rect.size.width, rect.size.height, 0xFF222233);
        fb_draw_img(&ctx, 20, 20, img, img_info.width, img_info.height);
        // fb_draw_string(&ctx, "Print screen test", rect.point.x, rect.point.y, 2, 0xFFFFFFFF);
        commit_draw_ctx(&ctx);
    }
    return 1;
}
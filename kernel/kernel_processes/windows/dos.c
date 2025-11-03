#include "dos.h"
#include "kernel_processes/kprocess_loader.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "ui/uno/uno.h"
#include "input/input_dispatch.h"
#include "console/kio.h"
#include "math/math.h"
#include "graph/tres.h"
#include "launcher.h"
#include "dev/driver_base.h"
#include "dev/module_loader.h"

#define BORDER_SIZE 3

bool init_window_system(){
    return true;
}

size_t window_cmd_read(const char *path, void *buf, size_t size){
    return 0;
}

size_t window_cmd_write(const char *path, const void *buf, size_t size){
    kprintf("Received command");
    return 0;
}

driver_module window_module = (driver_module){
    .name = "dos",
    .mount = "/window",
    .version = VERSION_NUM(0, 1, 0, 0),

    .init = init_window_system,
    .fini = 0,

    .open = 0,
    .read = 0,
    .write = 0,

    .sread = window_cmd_read,
    .swrite = window_cmd_write,

    .readdir = 0,
};

void draw_window(gpu_point location, gpu_size size){
    gpu_point fixed_point = {location.x - BORDER_SIZE, location.y - BORDER_SIZE};
    gpu_size fixed_size = { size.width + BORDER_SIZE*2, size.height + BORDER_SIZE*2 };
    draw_ctx *ctx = gpu_get_ctx();
    DRAW(rectangle(ctx, (rect_ui_config){
        .border_size = 3,
        .border_color = BG_COLOR + 0x222222
    }, (common_ui_config){
        .point = fixed_point,
        .size = fixed_size,
        .background_color = BG_COLOR,
        .foreground_color = COLOR_WHITE,
    }),{
        
    });
}

int window_system(){
    load_module(&window_module);
    disable_visual();
    gpu_clear(BG_COLOR);
    gpu_point start_point = {0,0};
    bool drawing = false;
    while (1){
        if (mouse_button_pressed(LMB)){
            if (!drawing){
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
            create_window(fixed_point.x, fixed_point.y, size.width, size.height);
            draw_window(fixed_point, size);
            launch_launcher();
            drawing = false;
        }
        gpu_flush();
    }
    return 0;
}

process_t* create_windowing_system(){
    return create_kernel_process("dos", window_system, 0, 0);
}
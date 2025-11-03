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
#include "data_struct/linked_list.h"
#include "syscalls/syscalls.h"
#include "exceptions/irq.h"

#define BORDER_SIZE 3


typedef struct {
    uint16_t win_id;
    uint32_t x, y;
    uint32_t width, height;
} window_frame;

clinkedlist_t *window_list;

static uint16_t win_ids = 1;
bool dirty_windows = false;
bool res = false;

bool init_window_system(){
    window_list = clinkedlist_create();
    return true;
}

size_t window_cmd_read(const char *path, void *buf, size_t size){
    return 0;
}

int find_window(void *node, void *key){
    window_frame* frame = (window_frame*)node;
    uint16_t wid = *(uint16_t*)key;
    if (frame->win_id == wid) return 0;
    return -1;
}

size_t window_cmd_write(const char *path, const void *buf, size_t size){
    const char *wid_s = seek_to(path, '/');
    path = seek_to(wid_s, '/');
    uint64_t wid = parse_int_u64(wid_s, path - wid_s);
    kprintf("Received command %s for window %i",path,wid);
    if (size != 2 * sizeof(uint32_t)) return 0;
    uint32_t *new_size = (uint32_t*)buf;
    clinkedlist_node_t *node = clinkedlist_find(window_list, &wid, find_window);
    if (node && node->data){
        window_frame* frame = (window_frame*)node->data;
        kprintf("Previous window %ix%i",frame->width,frame->height);
        frame->width = new_size[0];
        frame->height = new_size[1];
        dirty_windows = true;
        res = true;
    }
    return size;
}

void create_win(uint32_t x,uint32_t y, uint32_t width,uint32_t height){
    if (win_ids == UINT16_MAX) return;
    window_frame *frame = malloc(sizeof(window_frame));
    frame->win_id = win_ids++;
    frame->width = width;
    frame->height = height;
    frame->x = x;
    frame->y = y;
    clinkedlist_push_front(window_list, frame);
    create_window(x, y, width, height);
    launch_launcher();
    dirty_windows = true;
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

void draw_window(window_frame *frame){
    gpu_point fixed_point = { frame->x - BORDER_SIZE, frame->y - BORDER_SIZE };
    gpu_size fixed_size = { frame->width + BORDER_SIZE*2, frame->height + BORDER_SIZE*2 };
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

static inline void redraw_win(void *node){
    window_frame* frame = (window_frame*)node;
    draw_window(frame);
}

int window_system(){
    // load_module(&window_module);//TODO: create a process-specific function for this, that keeps track of modules and unloads them on exit/crash
    init_window_system();
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
            create_win(fixed_point.x, fixed_point.y, size.width, size.height);
            drawing = false;
        }
        if (dirty_windows){
            disable_interrupt();
            gpu_clear(BG_COLOR);
            res = false;
            clinkedlist_for_each(window_list, redraw_win);
            dirty_windows = false;
            enable_interrupt();
        }
        gpu_flush();
    }
    return 0;
}

process_t* create_windowing_system(){
    return create_kernel_process("dos", window_system, 0, 0);
}
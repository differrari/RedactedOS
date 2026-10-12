#include "wincomp.h"
#include "graphic_types.h"
#include "graph/tres.h"
#include "graph/graphics.h"
#include "math/math.h"
#include "console/kio.h"

int_point current_win_offset = {};

void new_managed_window(){
    draw_ctx *cur = gpu_get_ctx();
    if (!cur) return;
    create_window(10 - current_win_offset.x, 10 - current_win_offset.y + MENU_HEIGHT, cur->width - 20, cur->height - 20 - MENU_HEIGHT);
    global_win_offset.x = current_win_offset.x;
    global_win_offset.y = current_win_offset.y;
    current_win_offset.x -= cur->width;
}

void new_aux_window(window_frame *main){
    draw_ctx *cur = gpu_get_ctx();
    if (!cur) return;

    string s = string_format("%i",main->pid);

    const char *args[] = {
        "debugger.elf",
        s.data
    };
    main->aux = create_window_prog(main->x, main->y+main->height, main->width, 200, "/boot/redos/system/debugger.red", 2, args);
    main->aux->is_aux = true;
    string_free(s);
}

void switch_focus(int8_t x, int8_t y){
    x = min(max(x,-1),1);
    y = min(max(y,-1),1);
    
    draw_ctx *cur = gpu_get_ctx();
    if (!cur) return;
    global_win_offset.x += cur->width * x;
    global_win_offset.y += cur->height * y;
    
    global_win_offset.x = (((int32_t)(global_win_offset.x/(int32_t)cur->width))*cur->width);
    global_win_offset.y = (((int32_t)(global_win_offset.y/(int32_t)cur->height))*cur->height);
    
    dirty_windows = true;
}
#include "wincomp.h"
#include "ui/graphic_types.h"
#include "graph/tres.h"
#include "graph/graphics.h"
#include "math/math.h"
#include "console/kio.h"

int_point current_win_offset = {};

void new_managed_window(){
    draw_ctx *cur = gpu_get_ctx();
    create_window(10 - current_win_offset.x, 10 - current_win_offset.y, cur->width - 20, cur->height - 20);
    global_win_offset.x = current_win_offset.x;
    current_win_offset.x -= cur->width;
}

void switch_focus(int8_t x, int8_t y){
    x = min(max(x,-1),1);
    y = min(max(y,-1),1);
    
    draw_ctx *cur = gpu_get_ctx();
    global_win_offset.x += cur->width * x;
    global_win_offset.y += cur->height * y;
    
    global_win_offset.x = (((int32_t)(global_win_offset.x/(int32_t)cur->width))*cur->width);
    global_win_offset.y = (((int32_t)(global_win_offset.y/(int32_t)cur->height))*cur->height);
    
    dirty_windows = true;
}
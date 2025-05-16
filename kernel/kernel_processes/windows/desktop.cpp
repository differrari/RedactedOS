#include "desktop.hpp"
#include "console/kio.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "input/input_dispatch.h"

#define MAX_COLS 3
#define MAX_ROWS 3

Desktop::Desktop(){
    
}

void Desktop::draw_desktop(){
    if (!await_gpu()) return;
    keypress kp;
    if (sys_read_input_current(&kp)){
        for (int i = 0; i < 6; i++){
            char key = kp.keys[i];
            if (kp.keys[i] == KEY_ARROW_RIGHT){
                selected.x = (selected.x + 1) % MAX_COLS;
            }
            if (kp.keys[i] == KEY_ARROW_LEFT){
                selected.x = (selected.x - 1 + MAX_COLS) % MAX_COLS;
            } 
            if (kp.keys[i] == KEY_ARROW_DOWN){
                selected.y = (selected.y + 1) % MAX_ROWS;
            } 
            if (kp.keys[i] == KEY_ARROW_UP){
                selected.y = (selected.y - 1 + MAX_ROWS) % MAX_ROWS;
            } 
        }
    }
    gpu_clear(BG_COLOR);
    for (uint32_t column = 0; column < MAX_COLS; column++){
        for (uint32_t row = 0; row < MAX_ROWS; row++){
            draw_tile(column, row);
        }
    }
    gpu_flush();
}

bool Desktop::await_gpu(){
    if (!gpu_ready())
        return false;
    if (!ready){
        disable_visual();
        sys_focus_current();
        gpu_size screen_size = gpu_get_screen_size();
        tile_size = {screen_size.width/MAX_COLS - 20, screen_size.height/(MAX_ROWS+1) - 20};
        ready = true;
    }
    return ready;
}

void Desktop::draw_tile(uint32_t column, uint32_t row){
    bool sel = selected.x == column && selected.y == row;

    int border = 4;

    if (sel)
        gpu_fill_rect((gpu_rect){10 + ((tile_size.width + 10)*column), 50 + ((tile_size.height + 10) *row), tile_size.width, tile_size.height}, BG_COLOR+0x333333);
    gpu_fill_rect((gpu_rect){10 + ((tile_size.width + 10)*column)+ (sel ? border : 0), 50 + ((tile_size.height + 10) *row) + (sel ? border : 0), tile_size.width - (sel ? border * 2 : 0), tile_size.height - (sel ? border * 2 : 0)}, BG_COLOR+0x111111);
}
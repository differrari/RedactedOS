#include "bootscreen.h"
#include "../kprocess_loader.h"
#include "console/kio.h"
#include "graph/graphics.h"
#include "kstring.h"
#include "ram_e.h"
#include "theme/theme.h"
#include "interrupts/exception_handler.h"
#include "input/input_dispatch.h"
#include "process/scheduler.h"
#include "math/math.h"

__attribute__((section(".data.kcoreprocesses")))
static uint64_t randomNumber = 0;

__attribute__((section(".text.kcoreprocesses")))
void boot_draw_name(gpu_point screen_middle,int xoffset, int yoffset){
    const char* name = BOOTSCREEN_TEXT;
    kstring s = string_l(name);
    int scale = 2;
    uint32_t char_size = gpu_get_char_size(scale);
    int mid_offset = (s.length/2) * char_size;
    int xo = screen_middle.x - mid_offset + xoffset;
    int yo = screen_middle.y + yoffset;
    gpu_fill_rect((gpu_rect){xo,yo, char_size * s.length, char_size},BG_COLOR);
    gpu_draw_string(s, (gpu_point){xo, yo}, scale, 0xFFFFFF);
    temp_free(s.data,s.length);
}

__attribute__((section(".rodata.kcoreprocesses")))
gpu_point offsets[BOOTSCREEN_NUM_SYMBOLS] = BOOTSCREEN_OFFSETS;

__attribute__((section(".text.kcoreprocesses")))
gpu_point boot_calc_point(gpu_point offset, int sizes[4], gpu_size screen_size, gpu_point screen_middle){
    bool x0 = offset.x == 0;
    bool y0 = offset.y == 0;
    bool ui = !(abs(offset.x) - 1);
    bool ul = !(abs(offset.y) - 1);
    int xs = sign(offset.x);
    int ys = sign(offset.y);
    int xloc = BOOTSCREEN_PADDING + (x0 ? 0 : (ui ? sizes[3] : sizes[1]));
    int yloc = BOOTSCREEN_PADDING + (y0 ? 0 : (ul ? sizes[0] : sizes[2]));
    return (gpu_point){screen_middle.x + (xs * xloc) - (ui ? BOOTSCREEN_ASYMM.x : 0),  screen_middle.y + (ys * yloc) - (ul ? BOOTSCREEN_ASYMM.y : 0)};
}

__attribute__((section(".text.kcoreprocesses")))
void bootscreen(){
    disable_visual();
    sys_focus_current();
    while (1)
    {
        gpu_clear(BG_COLOR);
        gpu_size screen_size = gpu_get_screen_size();
        gpu_point screen_middle = {screen_size.width/2,screen_size.height/2};
        int sizes[4] = {BOOTSCREEN_INNER_X_CONST,screen_size.width/BOOTSCREEN_OUTER_X_DIV,screen_size.height/BOOTSCREEN_UPPER_Y_DIV,BOOTSCREEN_LOWER_Y_CONST};
        
        gpu_point current_point = boot_calc_point(offsets[BOOTSCREEN_NUM_SYMBOLS-1],sizes,screen_size,screen_middle);
        for (int i = 0; i < BOOTSCREEN_NUM_STEPS; i++){
            gpu_point offset = offsets[i];
            gpu_point next_point = boot_calc_point(offset,sizes,screen_size,screen_middle);
            int xlength = abs(current_point.x - next_point.x);
            int ylength = abs(current_point.y - next_point.y);
            int steps = xlength > ylength ? xlength : ylength;
            for (int i = 0; i <= steps; i++) {
                gpu_point interpolated = {
                    lerp(i, current_point.x, next_point.x, steps),
                    lerp(i, current_point.y, next_point.y, steps)
                };
                gpu_draw_pixel(interpolated, 0xFFFFFF);
                keypress kp;
                if (sys_read_input_current(&kp))
                    if (kp.keys[0] != 0){
                        stop_current_process();
                    }
                boot_draw_name(screen_middle, 0, BOOTSCREEN_PADDING + sizes[2] + 10);
                gpu_flush();
            }
            current_point = next_point;
        }
    }
}

process_t* start_bootscreen(){
    return create_kernel_process("bootscreen",bootscreen);
}
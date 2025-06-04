#include "bootscreen.h"
#include "../kprocess_loader.h"
#include "console/kio.h"
#include "graph/graphics.h"
#include "std/string.h"
#include "memory/kalloc.h"
#include "theme/theme.h"
#include "interrupts/exception_handler.h"
#include "input/input_dispatch.h"
#include "process/scheduler.h"
#include "math/math.h"
#include "syscalls/syscalls.h"

__attribute__((section(".text.kcoreprocesses")))
void boot_draw_name(gpu_point screen_middle,int xoffset, int yoffset){
    const char* name = BOOTSCREEN_TEXT;
    string s = string_l(name);
    int scale = 2;
    uint32_t char_size = gpu_get_char_size(scale);
    int mid_offset = ((s.length/BOOTSCREEN_NUM_LINES) * char_size)/2;
    int xo = screen_middle.x - mid_offset + xoffset;
    int yo = screen_middle.y + yoffset;
    gpu_fill_rect((gpu_rect){{xo,yo}, {char_size * (s.length/BOOTSCREEN_NUM_LINES), char_size * BOOTSCREEN_NUM_LINES}},BG_COLOR);
    gpu_draw_string(s, (gpu_point){xo, yo}, scale, 0xFFFFFF);
    free(s.data,s.mem_length);
}

__attribute__((section(".rodata.kcoreprocesses")))
const gpu_point offsets[BOOTSCREEN_NUM_SYMBOLS] = BOOTSCREEN_OFFSETS;

__attribute__((section(".text.kcoreprocesses")))
gpu_point boot_calc_point(gpu_point offset, gpu_size screen_size, gpu_point screen_middle){
    int xoff = (screen_size.width/BOOTSCREEN_DIV) * offset.x;
    int yoff = (screen_size.height/BOOTSCREEN_DIV) * offset.y;
    int xloc = BOOTSCREEN_PADDING + xoff;
    int yloc = BOOTSCREEN_PADDING + yoff;
    return (gpu_point){screen_middle.x + xloc - (abs(offset.x) == 1 ? BOOTSCREEN_ASYMM.x : 0),  screen_middle.y + yloc - (abs(offset.y) == 1 ? BOOTSCREEN_ASYMM.y : 0)};
}

void boot_draw_lines(gpu_point current_point, gpu_point next_point, gpu_size size, int num_lines, int separation){
    int steps = 0;
    for (int j = 0; j < num_lines; j++) {
        gpu_point ccurrent = current_point;
        gpu_point cnext = next_point;
        if (current_point.x != 0 && current_point.x < size.width)
            ccurrent.x += (separation * j);
        if (next_point.x != 0 && next_point.x < size.width)
            cnext.x += (separation * j);
        if (current_point.y != 0 && current_point.y < size.height)
            ccurrent.y -= (separation * j);
        if (next_point.y != 0 && next_point.y < size.height)
            cnext.y -= (separation * j);
        int xlength = abs(ccurrent.x - cnext.x);
        int ylength = abs(ccurrent.y - cnext.y);
        int csteps = xlength > ylength ? xlength : ylength;
        if (csteps > steps) steps = csteps;
    }
    
    for (int i = 0; i <= steps; i++) {
        for (int j = 0; j < num_lines; j++){
            gpu_point ccurrent = current_point;
            gpu_point cnext = next_point;
            if (current_point.x != 0 && current_point.x < size.width)
                ccurrent.x += (separation * j);
            if (next_point.x != 0 && next_point.x < size.width)
                cnext.x += (separation * j);
            if (current_point.y != 0 && current_point.y < size.height)
                ccurrent.y -= (separation * j);
            if (next_point.y != 0 && next_point.y < size.height)
                cnext.y -= (separation * j);
            int xlength = abs(ccurrent.x - cnext.x);
            int ylength = abs(ccurrent.y - cnext.y);
            int csteps = xlength > ylength ? xlength : ylength;
            if (csteps > i){
                gpu_point interpolated = {
                    lerp(i, ccurrent.x, cnext.x, csteps),
                    lerp(i, ccurrent.y, cnext.y, csteps)
                };
                gpu_draw_pixel(interpolated, 0xFFFFFF);
            }
        }
        keypress kp;
        if (sys_read_input_current(&kp))
            if (kp.keys[0] != 0){
                stop_current_process();
            }
        gpu_flush();
        // sleep(0);
    }
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
        
        gpu_point current_point = boot_calc_point(offsets[BOOTSCREEN_NUM_SYMBOLS-1],screen_size,screen_middle);
        for (int i = 0; i < BOOTSCREEN_NUM_STEPS; i++){
            gpu_point offset = offsets[i];
            gpu_point next_point = boot_calc_point(offset,screen_size,screen_middle);
            boot_draw_name(screen_middle, 0, BOOTSCREEN_PADDING + screen_size.height/BOOTSCREEN_UPPER_Y_DIV + 10);
            boot_draw_lines(current_point, next_point, screen_size, BOOTSCREEN_REPEAT, 5);
            current_point = next_point;
        }
        sleep(1000);
    }
}

process_t* start_bootscreen(){
    return create_kernel_process("bootscreen",bootscreen);
}
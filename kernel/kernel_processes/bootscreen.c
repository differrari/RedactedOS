#include "bootscreen.h"
#include "process/kprocess_loader.h"
#include "console/kio.h"
#include "graph/graphics.h"

int abs(int n){
    return n < 0 ? -n : n;
}

__attribute__((section(".text.kbootscreen")))
void bootscreen(){
    disable_visual();
    // while (1)
    // {
        gpu_clear(0);
        size screen_size = gpu_get_screen_size();
        point screen_middle = {screen_size.width/2,screen_size.height/2};
        int sizes[4] = {30,screen_size.width/5,screen_size.height/3,40};
        int padding = 10;
        int yoffset = 50;
        point current_point = {screen_middle.x-padding-sizes[1],screen_middle.y-padding-yoffset-sizes[0]};
        for (int i = 0; i < 12; i++){
            int ys = i > 5 ? -1 : 1;
            bool ui = (i % 6) != 0 && (i % 6) != 5;
            bool ul = (i/2) % 2 == 0;
            bool xn = (i/3) % 3 == 0;
            if (i >= 6){
                ul = !ul;
            }
            int xs = xn ? -1 : 1;
            int xloc = padding + (ui ? sizes[3] : sizes[1]);
            int yloc = padding + (ul ? sizes[0] : sizes[2]);
            point next_point = {screen_middle.x + (xs * xloc),  screen_middle.y + (ys * yloc) - (ul ? yoffset : 0)};
            int xlength = abs(current_point.x - next_point.x);
            int ylength = abs(current_point.y - next_point.y);
            kprintf("[%i] x will be %i y will be %i between %i,%i and %i,%i ys=%i ui=%i ul=%i xs%i",i,xlength,ylength,current_point.x,current_point.y, next_point.x,next_point.y, ys,ui,ul,xs);
            // for (int x = current_point.x; x < xlength; x++){
            //     for (int y = current_point.y; y < ylength; y++){
                    //Draw the line interpolating
                    gpu_draw_line(current_point,next_point,0xFFFFFF);
            //     }    
            // }
            current_point = next_point;
        }
        kprintf("Hello world");
    // }
    while (1) {}
    
}

void start_bootscreen(){

    extern uint8_t kbootscreen_start;
    extern uint8_t kbootscreen_end;
    // extern uint8_t kbootscreen_rodata_start;
    // extern uint8_t kbootscreen_rodata_end;
    create_kernel_process(bootscreen, (uint64_t)&kbootscreen_end - (uint64_t)&kbootscreen_start/*, (uint64_t)&kbootscreen_start, (void*)0x0, 0*/);
}
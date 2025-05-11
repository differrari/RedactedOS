#include "bootscreen.h"
#include "process/kprocess_loader.h"
#include "console/kio.h"
#include "graph/graphics.h"
#include "kstring.h"
#include "ram_e.h"
#include "exception_handler.h"

int abs(int n){
    return n < 0 ? -n : n;
}

int lerp(int step, int a, int b){
    return (a + step * (a < b ? 1 : -1));
}

static uint64_t randomNumber = 0;

__attribute__((section(".text.kbootscreen")))
void boot_draw_name(point screen_middle,int xoffset, int yoffset){
    const char* name = "JesOS - The Christian Operative System - %i%";
    uint64_t *i = &randomNumber;
    kstring s = string_format_args(name, i, 1);
    int scale = 2;
    uint32_t char_size = gpu_get_char_size(scale);
    int mid_offset = (s.length/2) * char_size;
    int xo = screen_middle.x - mid_offset + xoffset;
    int yo = screen_middle.y + yoffset;
    gpu_fill_rect((rect){xo,yo, char_size * s.length, char_size},0x0);
    gpu_draw_string(s, (point){xo, yo}, scale, 0xFFFFFF);
    temp_free(s.data,256);
}

__attribute__((section(".text.kbootscreen")))
void bootscreen(){
    disable_visual();
    while (1)
    {
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
            if (i >= 6)
                ul = !ul;
            int xs = xn ? -1 : 1;
            int xloc = padding + (ui ? sizes[3] : sizes[1]);
            int yloc = padding + (ul ? sizes[0] : sizes[2]);
            point next_point = {screen_middle.x + (xs * xloc),  screen_middle.y + (ys * yloc) - (ul ? yoffset : 0)};
            int xlength = abs(current_point.x - next_point.x);
            int ylength = abs(current_point.y - next_point.y);
            for (int x = 0; x <= xlength; x++){
                for (int y = 0; y <= ylength; y++){
                    point interpolated = {lerp(x,current_point.x,next_point.x),lerp(y,current_point.y,next_point.y)};
                    gpu_draw_pixel(interpolated,0xFFFFFF);
                    for (int k = 0; k < 1000000; k++){}
                }    
                boot_draw_name(screen_middle,0,padding + sizes[2] + 10);
            }
            randomNumber += 50;
            randomNumber %= 100;
            current_point = next_point;
        }
    }
    
}

void start_bootscreen(){
    create_kernel_process(bootscreen);
}
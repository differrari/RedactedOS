#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "math/math.h"

#define CELL_ON (1 << 0)
#define CELL_FLIP_ON (1 << 1)
#define CELL_FLIP_OFF (1 << 2)

#define BG_COLOR 0
#define CELL_COLOR 0xFFB4DD13

#define SCALE 10

#define CELL_AT(x,y) grid[((y) * size.width) + (x)]

bool *grid;
gpu_size size;

void check_neighbors(){
    for (int x = 0; x < (int)size.width; x++){
        for (int y = 0; y < (int)size.height; y++){
            int neighbors = 0;
            for (int nx = -1; nx <= 1; nx++){
                for (int ny = -1; ny <= 1; ny++){
                    if ((nx == 0 && ny == 0) || x + nx < 0 || y + ny < 0 || x + nx > (int)size.width || y + ny > (int)size.height)
                        continue;
                    // printf("%ix%i %i",x + nx, y + ny, ((ny+y) * size.width) + (nx+x), grid[((ny+y) * size.width) + (nx+x)]);
                    if (grid[((ny+y) * size.width) + (nx+x)] & CELL_ON && !(grid[((ny+y) * size.width) + (nx+x)] & (CELL_FLIP_ON | CELL_FLIP_OFF)))
                        neighbors++;
                }
            }
            if (CELL_AT(x, y) & CELL_ON && (neighbors < 2 || neighbors > 3))
                CELL_AT(x, y) = CELL_FLIP_OFF;
            if (!(CELL_AT(x, y) & CELL_ON) && neighbors == 3)
                CELL_AT(x, y) = CELL_FLIP_ON | CELL_ON;
        }
    }
}

void proc_func() {
    draw_ctx ctx = {};
    request_draw_ctx(&ctx);
    rng_t rng;
    rng_seed(&rng, get_time());
    size = (gpu_size){max(ctx.width/SCALE,100),max(ctx.height/SCALE,100)};
    printf("Window size %xx%x", size.width,size.height);
    // gpu_rect rect = (gpu_rect){{10,10},{size.width-20,size.height-20}};
    grid = (bool*)malloc(size.height * size.width);
    for (uint32_t x = 0; x < size.width; x++){
        for (uint32_t y = 0; y < size.height; y++){
            if (rng_next8(&rng) % 10 == 0)
                CELL_AT(x, y) = CELL_FLIP_ON | CELL_ON;
        }
    }
    fb_clear(&ctx, BG_COLOR);
    while (1) {
        keypress kp = {};
        // printf("Print console test %f", (get_time()/1000.f));
        if (read_key(&kp))
            if (kp.keys[0] == KEY_ESC)
                halt(0);
        for (uint32_t x = 0; x < size.width; x++){
            for (uint32_t y = 0; y < size.height; y++){
                if (CELL_AT(x, y) & CELL_FLIP_OFF){
                    fb_fill_rect(&ctx, x * SCALE, y * SCALE, SCALE, SCALE, BG_COLOR);
                    CELL_AT(x, y) &= ~(CELL_FLIP_OFF);
                }
                else if (CELL_AT(x, y) & CELL_FLIP_ON){
                    fb_fill_rect(&ctx, x * SCALE, y * SCALE, SCALE, SCALE, CELL_COLOR);
                    CELL_AT(x, y) &= ~(CELL_FLIP_ON);
                }
            }
        }
        // fb_fill_rect(&ctx, rect.point.x, rect.point.y, rect.size.width, rect.size.height, 0xFF222233);
        // fb_draw_string(&ctx, "Print screen test", rect.point.x, rect.point.y, 2, 0xFFFFFFFF);
        commit_draw_ctx(&ctx);
        check_neighbors();
        sleep(100);
    }
    halt(1);
}
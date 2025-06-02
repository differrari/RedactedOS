#include "default_process.h"
#include "types.h"
#include "std/syscalls/syscalls.h"
#include "input_keycodes.h"

static const char fmt[] = "Process %i";//Note: So long as we keep loading this process inside the kernel, we'll need to keep strings outside functions so we can reliably indicate their section
static const char woah[] = "Woah";

void proc_func() {
    uint64_t j = 0;
    gpu_size* size = gpu_screen_size();
    gpu_rect rect = (gpu_rect){{10,10},{size->width-20,size->height-20}};
    while (1) {
        keypress kp;
        printf(fmt, j++);
        while (read_key(&kp)){
            if (kp.keys[0] == KEY_ESC)
                halt();
        }
        clear_screen(0xFF00FF);
        draw_primitive_rect(&rect, 0xFFFFFF);
        draw_primitive_text(woah,&rect.point,2, 0x0);
        gpu_flush_data();
    }
}
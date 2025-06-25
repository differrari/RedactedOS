#include "default_process.h"
#include "types.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"

void proc_func() {
    uint64_t j = 0;
    gpu_size* size = gpu_screen_size();
    gpu_rect rect = (gpu_rect){{10,10},{size->width-20,size->height-20}};
    while (1) {
        keypress kp;
        printf("Print console test %f", (get_time()/1000.f));
        while (read_key(&kp)){
            if (kp.keys[0] == KEY_ESC)
                halt();
        }
        clear_screen(0xFFFFFF);
        draw_primitive_rect(&rect, 0x222233);
        string s = string_l("Print screen test");
        draw_primitive_string(&s,&rect.point,2, 0xFFFFFF);
        free(s.data,s.mem_length);
        gpu_flush_data();
    }
}
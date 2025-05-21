#include "default_process.h"
#include "types.h"
#include "process_loader.h"
#include "std/syscalls/syscalls.h"

__attribute__((section(".rodata.proc1")))
static const char fmt[] = "Process %i";//Note: So long as we keep loading this process inside the kernel, we'll need to keep strings outside functions so we can reliably indicate their section
__attribute__((section(".rodata.proc1")))
static const char woah[] = "Woah";

__attribute__((section(".text.proc1")))
void proc_func() {
    uint64_t j = 0;
    gpu_size* size = gpu_screen_size();
    gpu_rect rect = (gpu_rect){10,10,size->width-20,size->height-20};
    request_focus();
    while (1) {
        keypress *kp = (keypress*)malloc(sizeof(keypress));
        if (read_key(kp)){
            if (kp->keys[0] != 0)
                printf(fmt, j++);
        }
        free(kp, sizeof(keypress));
        clear_screen(0xFF00FF);
        draw_primitive_rect(&rect, 0xFFFFFF);
        draw_primitive_text(woah,&rect.point,2, 0x0);
        gpu_flush_data();
    }
}

process_t* default_processes(){
    extern uint8_t proc_1_start;
    extern uint8_t proc_1_end;
    extern uint8_t proc_1_rodata_start;
    extern uint8_t proc_1_rodata_end;

    return create_process("testprocess",proc_func, (uint64_t)&proc_1_end - (uint64_t)&proc_1_start, (uint64_t)&proc_1_start, (void*)&fmt, (uint64_t)&proc_1_rodata_end - (uint64_t)&proc_1_rodata_start);
}
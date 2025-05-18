#include "default_process.h"
#include "types.h"
#include "process_loader.h"
#include "syscalls/syscalls.h"

__attribute__((section(".rodata.proc1")))
static const char fmt[] = "Process %i";//Note: So long as we keep loading this process inside the kernel, we'll need to keep strings outside functions so we can reliably indicate their section

__attribute__((section(".text.proc1")))
void proc_func() {
    uint64_t j = 0;
    while (1) {
        printf(fmt, j++);
        for (int i = 0; i < 100000000; i++);
    }
}

void default_processes(){
    extern uint8_t proc_1_start;
    extern uint8_t proc_1_end;
    extern uint8_t proc_1_rodata_start;
    extern uint8_t proc_1_rodata_end;

    create_process("testproc1",proc_func, (uint64_t)&proc_1_end - (uint64_t)&proc_1_start, (uint64_t)&proc_1_start, (void*)&fmt, (uint64_t)&proc_1_rodata_end - (uint64_t)&proc_1_rodata_start);
    create_process("testproc2", proc_func, (uint64_t)&proc_1_end - (uint64_t)&proc_1_start, (uint64_t)&proc_1_start, (void*)&fmt, (uint64_t)&proc_1_rodata_end - (uint64_t)&proc_1_rodata_start);
}
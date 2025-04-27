#include "default_process.h"
#include "types.h"
#include "scheduler.h"
#include "console/kio.h"
#include "syscalls/syscalls.h"

__attribute__((section(".rodata.proc1")))
static const char fmt[] = "Process %i";
__attribute__((section(".data.proc1")))
static uint64_t j = 1;

__attribute__((section(".text.proc1")))
void proc_func() {
    while (1) {
        register uint64_t x0 asm("x0") = (uint64_t)&fmt;
        register uint64_t x1 asm("x1") = (uint64_t)&j;
        register uint64_t x2 asm("x2") = 1;
        register uint64_t x8 asm("x8") = 3;

        asm volatile(
            "svc #3"
            :
            : "r"(x0), "r"(x1), "r"(x2), "r"(x8)
            : "memory"
        );
        j++;
    }

}

void default_processes(){
    extern uint8_t proc_1_start;
    extern uint8_t proc_1_end;
    extern uint8_t proc_1_rodata_start;
    extern uint8_t proc_1_rodata_end;

    kprintf("Proc starts at %h and ends at %h",(uint64_t)&proc_1_start, (uint64_t)&proc_1_end);
    kprintf("Data starts at %h and ends at %h",(uint64_t)&proc_1_rodata_start, (uint64_t)&proc_1_rodata_end);

    create_process(proc_func, (uint64_t)&proc_1_end - (uint64_t)&proc_1_start, (uint64_t)&proc_1_start, (void*)&fmt, (uint64_t)&proc_1_rodata_end - (uint64_t)&proc_1_rodata_start);
    create_process(proc_func, (uint64_t)&proc_1_end - (uint64_t)&proc_1_start, (uint64_t)&proc_1_start, (void*)&fmt, (uint64_t)&proc_1_rodata_end - (uint64_t)&proc_1_rodata_start);
}
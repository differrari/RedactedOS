#include "monitor_processes.h"
#include "process/scheduler.h"
#include "../kprocess_loader.h"
#include "console/kio.h"

__attribute__((section(".text.kcoreprocesses")))
char* parse_proc_state(int state){
    switch (state)
    {
    case STOPPED:
        return "Stopped";

    case READY:
    case RUNNING:
    case BLOCKED:
        return "Running";
    
    default:
        break;
    }
}

__attribute__((section(".text.kcoreprocesses")))
void print_process_info(){
    while (1){
        uint16_t proc_count = process_count();
        process_t *processes = get_all_processes();
        for (int i = 0; i < MAX_PROCS; i++){
            process_t *proc = &processes[i];
            if (proc->id != 0 && proc->state != STOPPED){
                kprintf("Process: %i. [Status: %s]",proc->id,(uint64_t)parse_proc_state(proc->state));
                kprintf("Stack: %h (%h). SP: %h",proc->stack, proc->stack_size, proc->sp);
                kprintf("Flags: %h", proc->spsr);
                kprintf("PC: %h",proc->pc);
            }
        }
        for (int i = 0; i < 100000000; i++);
    }
}

void start_process_monitor(){
    create_kernel_process(print_process_info);
}
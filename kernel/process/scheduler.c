#include "scheduler.h"
#include "console/kio.h"
#include "ram_e.h"
#include "proc_allocator.h"
#include "gic.h"
#include "console/serial/uart.h"

extern void save_context(process_t* proc);
extern void save_pc_interrupt(process_t* proc);
extern void restore_context(process_t* proc);

#define MAX_PROCS 16
process_t processes[MAX_PROCS];
int current_proc = 0;
int proc_count = 0;

void save_context_registers(){
    save_context(&processes[current_proc]);
}

void save_return_address_interrupt(){
    save_pc_interrupt(&processes[current_proc]);
}

void switch_proc(ProcSwitchReason reason) {
    // kprintf_raw("Stopping execution of process %i at %h",current_proc, processes[current_proc].sp);
    if (proc_count == 0)
        return;
    int next_proc = (current_proc + 1) % proc_count;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % proc_count;
        if (next_proc == current_proc)
            return;
    }
    
    current_proc = next_proc;
    // kprintf_raw("Resuming execution of process %i at %h",current_proc, processes[current_proc].sp);
    process_restore();
}

void process_restore(){
    restore_context(&processes[current_proc]);
}

void start_scheduler(){
    disable_interrupt();
    timer_init(1);
    switch_proc(YIELD);
}

int get_current_proc(){
    return current_proc;
}

process_t* init_process(){
    if (proc_count >= MAX_PROCS) return 0;

    process_t* proc = &processes[proc_count];
    proc->id = proc_count++;
    return proc;
}
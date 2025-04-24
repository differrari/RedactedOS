#include "scheduler.h"
#include "console/kio.h"
#include "ram_e.h"
#include "proc_allocator.h"
#include "gic.h"

extern void save_context(process_t* proc);
extern void save_pc_interrupt(process_t* proc);
extern void restore_context(process_t* proc);
extern void restore_context(process_t* proc);
extern void restore_context_yield(process_t* proc);
extern void restore_pc_interrupt(process_t* proc);

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

void restore_return_address_interrupt(){
    restore_pc_interrupt(&processes[current_proc]);
}

void switch_proc(ProcSwitchReason reason) {
    if (proc_count == 0)
        return;
    int next_proc = (current_proc + 1) % proc_count;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % proc_count;
        if (next_proc == current_proc)
            return;
    }

    current_proc = next_proc;
    if (reason == YIELD)
        restore_context_yield(&processes[current_proc]);
    else 
        restore_context(&processes[current_proc]);
}

process_t* create_process(void (*func)()) {
    if (proc_count >= MAX_PROCS) return 0;

    process_t* proc = &processes[proc_count];
    proc->sp = (uint64_t)alloc_proc_mem(0x1000);
    proc->pc = (uint64_t)func;
    proc->spsr = 0x3C5; // clean flags
    proc->state = READY;
    proc->id = proc_count++;
    
    return proc;
}

void start_scheduler(){
    timer_init(1);
    switch_proc(YIELD);
}

int get_current_proc(){
    return current_proc;
}
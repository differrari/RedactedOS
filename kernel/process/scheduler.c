#include "scheduler.h"
#include "console/kio.h"
#include "ram_e.h"
#include "proc_allocator.h"
#include "interrupts/gic.h"
#include "console/serial/uart.h"
#include "input/input_dispatch.h"
#include "interrupts/exception_handler.h"

extern void save_context(process_t* proc);
extern void save_pc_interrupt(process_t* proc);
extern void restore_context(process_t* proc);

#define MAX_PROCS 16
process_t processes[MAX_PROCS];
uint16_t current_proc = 0;
uint16_t proc_count = 0;
uint16_t next_proc_index = 1;

void save_context_registers(){
    save_context(&processes[current_proc]);
}

void save_return_address_interrupt(){
    save_pc_interrupt(&processes[current_proc]);
}

//TODO: Processes can currently exit and just crash the whole system with an EL1 Sync exception trying to read from 0x0. Better than continuing execution past bounds but still not great
void switch_proc(ProcSwitchReason reason) {
    // kprintf_raw("Stopping execution of process %i at %h",current_proc, processes[current_proc].spsr);
    if (proc_count == 0)
        panic("No processes active");
    int next_proc = (current_proc + 1) % next_proc_index;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % next_proc_index;
        if (next_proc == current_proc)
            return;
    }
    
    current_proc = next_proc;
    process_restore();
}

void process_restore(){
    // kprintf_raw("Resuming execution of process %i at %h",current_proc, processes[current_proc].spsr);
    restore_context(&processes[current_proc]);
}

void start_scheduler(){
    disable_interrupt();
    timer_init(1);
    switch_proc(YIELD);
}

process_t* get_current_proc(){
    return &processes[current_proc];
}

process_t* get_proc_by_pid(uint16_t pid){
    for (int i = 0; i < MAX_PROCS; i++)
        if (processes[i].id == pid)
            return &processes[i];
    return NULL;
}

uint16_t get_current_proc_pid(){
    return processes[current_proc].id;
}

void reset_process(process_t *proc){
    proc->sp = 0;
    free_proc_mem((void*)proc->stack-proc->stack_size,proc->stack_size);
    proc->pc = 0;
    proc->spsr = 0;
    for (int j = 0; j < 31; j++){
        proc->regs[j] = 0;
    }
    proc->input_buffer.read_index = 0;
    proc->input_buffer.write_index = 0;
    for (int k = 0; k < INPUT_BUFFER_CAPACITY; k++){
        proc->input_buffer.entries[k] = (keypress){0};
    }
}

process_t* init_process(){
    process_t* proc;
    if (next_proc_index >= MAX_PROCS){
        for (uint16_t i = 0; i < MAX_PROCS; i++){
            if (processes[i].state == STOPPED){
                proc = &processes[i];
                reset_process(proc);
                proc->state = READY;
                proc->id = i;
                return proc;
            }
        }
    }

    proc = &processes[next_proc_index];
    reset_process(proc);
    proc->id = next_proc_index++;
    proc->state = READY;
    proc_count++;
    return proc;
}

void stop_process(uint16_t pid){
    process_t *proc = &processes[pid];
    proc->state = STOPPED;
    sys_unset_focus();
    reset_process(proc);
    proc_count--;
    switch_proc(HALT);
}

void stop_current_process(){
    stop_process(current_proc);
}

uint16_t process_count(){
    return proc_count;
}

process_t *get_all_processes(){
    return processes;
}
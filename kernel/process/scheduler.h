#pragma once

#include "types.h"
#include "process.h"

typedef enum {
    INTERRUPT,
    YIELD,
    HALT,
} ProcSwitchReason;

#define MAX_PROCS 16

void switch_proc(ProcSwitchReason reason);
void start_scheduler();
void save_context_registers();
void save_return_address_interrupt();
process_t* init_process();
void process_restore();

process_t* get_current_proc();
process_t* get_proc_by_pid(uint16_t pid);
uint16_t get_current_proc_pid();

void stop_process(uint16_t pid);
void stop_current_process();

uint16_t process_count();
process_t *get_all_processes();
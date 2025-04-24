#pragma once

#include "types.h"
#include "process.h"

typedef enum {
    INTERRUPT,
    YIELD,
} ProcSwitchReason;

void switch_proc(ProcSwitchReason reason);
void start_scheduler();
int get_current_proc();
void save_context_registers();
void save_return_address_interrupt();
void restore_return_address_interrupt();
void relocate_code(void* dst, void* src, uint32_t size);
process_t* create_process(void (*func)(), uint64_t code_size, uint64_t func_base, void* data, uint64_t data_size);
void default_processes();
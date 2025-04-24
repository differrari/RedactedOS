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
process_t* create_process(void (*func)());
#pragma once

#include "types.h"
#include "process/process.h"
#include "files/system_module.h"

typedef enum {
    INTERRUPT,
    YIELD,
    HALT,
} ProcSwitchReason;

#define MAX_REUSABLE_EMPTY_PROCS 64

#define PROC_PRIORITY_FULL 25
#define PROC_PRIORITY_HIGH 10
#define PROC_PRIORITY_LOW  1

#ifdef __cplusplus
extern "C" {
#endif

void switch_proc(ProcSwitchReason reason);
bool start_scheduler();
void save_return_address_interrupt();
void init_main_process();
process_t* init_process();
void ready_process(process_t *proc);
void save_syscall_return(uint64_t value);
void process_restore();

void stop_process(uint16_t pid, int32_t exit_code);
void stop_current_process(int32_t exit_code);
void reset_process(process_t *proc);

void name_process(process_t *proc, const char *name);

void sleep_process(uint64_t msec);
void wake_processes();
void wake_process(process_t *proc);

bool load_process_module(process_t *p, system_module *m);

process_t* get_current_proc();
process_t* get_kernel_proc();
process_t* get_idle_proc();
bool scheduler_in_idle();
process_t* get_proc_by_pid(uint16_t pid);
uint16_t get_current_proc_pid();

uintptr_t get_current_heap();
bool get_current_privilege();
#ifdef __cplusplus
}
#endif

uint16_t process_count();
process_t *get_all_processes();

extern system_module scheduler_module;

extern char ksp[];

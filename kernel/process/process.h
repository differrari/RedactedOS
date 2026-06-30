#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "keyboard_input.h"
#include "net/network_types.h"
#include "files/system_module.h"
#include "memory/mm_process.h"
#include "graphic_types.h"
#include "signals/signals.h"
#include "environment/environment.h"

#define INPUT_BUFFER_CAPACITY 64
#define PACKET_BUFFER_CAPACITY 128
#define PROC_OUT_BUF 0x10000

typedef struct {
    volatile u32 write_index;
    volatile u32 read_index;
    keypress entries[INPUT_BUFFER_CAPACITY];
} input_buffer_t;

typedef struct {
    volatile u32 write_index;
    volatile u32 read_index;
    i8 entries[INPUT_BUFFER_CAPACITY];
} scroll_buffer_t;

typedef struct {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    kbd_event entries[INPUT_BUFFER_CAPACITY];
} event_buffer_t;

typedef struct {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    sizedptr entries[PACKET_BUFFER_CAPACITY];
} packet_buffer_t;

#define MAX_PROC_NAME_LENGTH 256

#define SIGNAL_BUFFER_CAPACITY 64

typedef struct {
    volatile u32 write_index;
    volatile u32 read_index;
    signal_info_t entries[SIGNAL_BUFFER_CAPACITY];
} signal_buffer_t;

typedef struct {
    u64 fs_id;
} system_permissions;

typedef enum { STOPPED, READY, RUNNING, BLOCKED } process_state;

typedef struct thread {
    uint64_t regs[31]; // x0–x30
    uintptr_t sp;
    uintptr_t pc;
    uint64_t spsr; 
    //Not used in context saving
    uintptr_t stack;
    uint64_t stack_size;
    u16 pid;
    u16 tid;
    process_state thread_state;
    struct thread *next;
} thread_t;

typedef struct process {
    thread_t main_thread;
    uint16_t id;
    bool sleeping;
    thread_t *current_thread;//NOTE: temporary
    bool suspended;
    uint64_t wake_at_msec;
    paddr_t heap_phys;
    kaddr_t output;
    size_t output_size;
    kaddr_t postmortem_output;
    size_t postmortem_output_size;
    uint16_t procfs_refs;
    bool pending_reset;
    file out_fd;
    int32_t exit_code;
    bool focused;
    paddr_t code;
    size_t code_size;
    uaddr_t va;
    page_index *alloc_map;
    draw_ctx graphics_ctx;
    process_state state;
    __attribute__((aligned(16))) input_buffer_t input_buffer;
    __attribute__((aligned(16))) event_buffer_t event_buffer;
    __attribute__((aligned(16))) packet_buffer_t packet_buffer;
    __attribute__((aligned(16))) scroll_buffer_t scroll_buffer;
    __attribute__((aligned(16))) signal_buffer_t signal_buffer;
    __attribute__((aligned(16))) thread_t signal_handlers[NUMBER_SIGNALS];
    uint8_t priority;
    system_permissions permissions;
    uint16_t win_id;
    uaddr_t win_fb_va;
    paddr_t win_fb_phys;
    uint64_t win_fb_size;
    char *bundle;
    char name[MAX_PROC_NAME_LENGTH];
    sizedptr debug_lines;
    sizedptr debug_line_str;
    system_module exposed_fs;
    thread_t fs_thread;
    mm_struct mm;
    int thread_count;
    environment_data environment;
    uptr shared_page;
    struct process *process_next;
} process_t;

static inline bool is_privileged(process_t *proc){
    return proc->main_thread.spsr & 0xf;
} 

//Helper functions for accessing registers mapped to scratch regs
#define PROC_X0 regs[0]
#define PROC_X1 regs[1]
#define PROC_X2 regs[2]
#define PROC_X3 regs[3]
#define PROC_X4 regs[4]
// #define PROC_FP regs[12]
// #define PROC_LR regs[11]
// #define PROC_SP regs[10]

#define PROC_PRIV spsr & 0x4

#ifdef __cplusplus
}
#endif
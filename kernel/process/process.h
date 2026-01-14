#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "keyboard_input.h"
#include "net/network_types.h"
#include "dev/driver_base.h"

#define INPUT_BUFFER_CAPACITY 64
#define PACKET_BUFFER_CAPACITY 128
#define PROC_OUT_BUF 0x10000

typedef struct {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    keypress entries[INPUT_BUFFER_CAPACITY];
} input_buffer_t;

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

typedef struct {
    //We use the addresses of these variables to save and restore process state
    uint64_t regs[31]; // x0–x30
    uintptr_t sp;
    uintptr_t pc;
    uint64_t spsr; 
    //Not used in process saving
    uint16_t id;
    uintptr_t stack;
    uintptr_t stack_phys;
    uint64_t stack_size;
    uintptr_t heap;
    uintptr_t heap_phys;
    uintptr_t output;
    size_t output_size;
    uintptr_t last_va_mapping;
    file out_fd;
    int32_t exit_code;
    bool focused;
    void *code;
    size_t code_size;
    bool use_va;
    uintptr_t *ttbr;
    uintptr_t va;
    enum process_state { STOPPED, READY, RUNNING, BLOCKED } state;
    __attribute__((aligned(16))) input_buffer_t input_buffer;
    __attribute__((aligned(16))) event_buffer_t event_buffer;
    __attribute__((aligned(16))) packet_buffer_t packet_buffer;
    uint8_t priority;
    uint16_t win_id;
    char *bundle;
    char name[MAX_PROC_NAME_LENGTH];
    sizedptr debug_lines;
    sizedptr debug_line_str;
} process_t;

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
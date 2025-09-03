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

typedef struct {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    keypress entries[INPUT_BUFFER_CAPACITY];
} input_buffer_t;

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
    uint64_t stack_size;
    uintptr_t heap;
    uintptr_t output;
    file out_fd;
    uint64_t exit_code;
    bool focused;
    enum process_state { STOPPED, READY, RUNNING, BLOCKED } state;
    input_buffer_t input_buffer;
    packet_buffer_t packet_buffer;
    char name[MAX_PROC_NAME_LENGTH];
} process_t;

//Helper functions for accessing registers mapped to scratch regs
#define PROC_X0 regs[14]
#define PROC_X1 regs[13]
#define PROC_X2 regs[8]
#define PROC_X3 regs[15]
#define PROC_X4 regs[3]
// #define PROC_FP regs[12]
// #define PROC_LR regs[11]
// #define PROC_SP regs[10]

#ifdef __cplusplus
}
#endif
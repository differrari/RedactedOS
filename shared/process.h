#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "keypress.h"

#define INPUT_BUFFER_CAPACITY 64

typedef struct {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    keypress entries[INPUT_BUFFER_CAPACITY];
} input_buffer_t;

#define MAX_PROC_NAME_LENGTH 256

typedef struct {
    //We use the addresses of these variables to save and restore process state
    uint64_t regs[31]; // x0–x30
    uint64_t sp;
    uint64_t pc;
    uint64_t spsr; 
    //Not used in process saving
    uint16_t id;
    uint64_t stack;
    uint64_t stack_size;
    bool focused;
    enum process_state { STOPPED, READY, RUNNING, BLOCKED } state;
    input_buffer_t input_buffer;
    char name[MAX_PROC_NAME_LENGTH];
} process_t;

#ifdef __cplusplus
}
#endif
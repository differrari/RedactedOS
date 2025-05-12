#pragma once

#include "types.h"
#include "keypress.h"

#define INPUT_BUFFER_CAPACITY 64

typedef struct {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    keypress entries[INPUT_BUFFER_CAPACITY];
} input_buffer_t;

typedef struct {
    uint64_t regs[31]; // x0–x30
    uint64_t sp;
    uint64_t pc;
    uint64_t spsr; 
    uint64_t id;
    enum { STOPPED, READY, RUNNING, BLOCKED } state;
    input_buffer_t input_buffer;
} process_t;

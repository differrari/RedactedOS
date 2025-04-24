#pragma once

#include "types.h"

typedef struct {
    uint64_t regs[31]; // x0–x30
    uint64_t sp;
    uint64_t pc;
    uint64_t spsr; 
    uint64_t id;
    enum { READY, RUNNING, BLOCKED } state;
} process_t;

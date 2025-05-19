#pragma once

#include "types.h"

typedef struct FreeBlock {
    uint64_t size;
    struct FreeBlock* next;
} FreeBlock;
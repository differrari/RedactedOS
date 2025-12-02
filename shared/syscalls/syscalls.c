#include "syscalls.h"
#include "std/string.h"
#include "math/math.h"
#include "std/memory.h"

void printf(const char *fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    char li[256]; 
    size_t n = string_format_va_buf(fmt, li, sizeof(li), args);
    va_end(args);
    if (n >= sizeof(li)) li[sizeof(li)-1] = '\0';
    printl(li);
}

void seek(file *descriptor, int64_t offset, SEEK_TYPE type){
    uint64_t new_cursor = descriptor->cursor;
    switch (type) {
        case SEEK_ABSOLUTE:
            new_cursor = (uint64_t)offset;
            break;
        case SEEK_RELATIVE:
            new_cursor += offset;
            break;
    }
    if (new_cursor > descriptor->size) return;//TODO: check what happens if we intentionally mess with the descriptor size before changing
    descriptor->cursor = new_cursor;
}

uintptr_t realloc(uintptr_t old_ptr, size_t old_size, size_t new_size){
    uintptr_t new_ptr = (uintptr_t)malloc(new_size);
    memcpy((void*)new_ptr, (void*)old_ptr, min(old_size,new_size));
    free_sized((void*)old_ptr, old_size);
    return new_ptr;
}
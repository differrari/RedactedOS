#include "allocator.hpp"
#include "syscalls/syscalls.h"

void* operator new(size_t size) { 
    return (void*)malloc(size);
}

void* operator new[](size_t size) { 
    return (void*)malloc(size);
}

void operator delete(void* ptr, size_t size) noexcept {
    free(ptr,size);
}
void operator delete[](void* ptr, size_t size) noexcept {
    free(ptr,size);
}
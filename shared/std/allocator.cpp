#include "allocator.hpp"
#include "syscalls/syscalls.h"
#include "console/kio.h"

void* operator new(size_t size) { 
    return (void*)malloc(size);
}

void* operator new[](size_t size) { 
    return (void*)malloc(size);
}

//TODO: We'll need to implement an unsized version of these, and keep track of size ourselves

void operator delete(void* ptr, size_t size) noexcept {
    free(ptr,size);
}
void operator delete[](void* ptr, size_t size) noexcept {
    free(ptr,size);
}
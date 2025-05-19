#include "allocator.hpp"
#include "memory/kalloc.h"

void* operator new(size_t size) { 
    return (void*)talloc(size); 
}

void* operator new[](size_t size) { 
    return (void*)talloc(size); 
}

void operator delete(void* ptr, size_t size) noexcept {
    temp_free(ptr,size);
}
void operator delete[](void* ptr, size_t size) noexcept {
    temp_free(ptr,size);
}
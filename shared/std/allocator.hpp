#pragma once 

#include "types.h"

void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* ptr, size_t size) noexcept;
void operator delete[](void* ptr, size_t size) noexcept;
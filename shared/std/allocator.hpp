#pragma once 

#include "types.h"

void* operator new(size_t size);
void* operator new[](size_t size);

void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;

void operator delete(void* ptr, size_t size) noexcept;
void operator delete[](void* ptr, size_t size) noexcept;

inline void* operator new(size_t, void* p) noexcept { return p; }
inline void* operator new[](size_t, void* p) noexcept { return p; }

inline void operator delete(void*, void*) noexcept {}
inline void operator delete[](void*, void*) noexcept {}
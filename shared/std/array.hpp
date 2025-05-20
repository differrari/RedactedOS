#pragma once

#include "types.h"

#include "allocator.hpp"
#include "syscalls/syscalls.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"

template<typename T>
class Array {
public:

    Array() : count(0), capacity(0), items(0) {
    }

    Array(uint32_t capacity) : count(0), capacity(capacity) {
        if (capacity == 0) {
            items = 0;
            return;
        }
        void *mem = (void*)malloc(sizeof(T) * capacity);
        items = reinterpret_cast<T*>(mem);
    }

    ~Array() {
        if (count == 0) return;
        for (uint32_t i = 0; i < count; i++)
            items[i].~T();
        ::operator delete(items, sizeof(T) * count);
    }

    bool add(const T& value) {
        if (count >= capacity) return false;
        items[count] = value;
        count++;
        return true;
    }

    T& operator[](uint32_t i) { return items[i]; }
    const T& operator[](uint32_t i) const { return items[i]; }
    uint32_t size() const { return count; }
    uint32_t max_size() const { return capacity; }

    T* items;
private:
    uint32_t count;
    uint32_t capacity;
};
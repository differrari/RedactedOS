#pragma once

#include "types.h"

#include "allocator.hpp"
#include "syscalls/syscalls.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"

template<typename T>
class IndexMap {
public:

    IndexMap() : count(0), capacity(0), items(0) {
    }

    IndexMap(uint32_t capacity) : count(0), capacity(capacity) {
        if (capacity == 0) {
            items = 0;
            return;
        }
        void *mem = (void*)malloc(sizeof(T) * capacity);
        items = reinterpret_cast<T*>(mem);
    }

    ~IndexMap() {
        //TODO: Deallocation has not been changed since this code was copied off Array
        if (count == 0) return;
        for (uint32_t i = 0; i < count; i++)
            items[i].~T();
        ::operator delete(items, sizeof(T) * count);
    }

    bool add(const uint32_t index, const T& value) {
        if (count >= capacity) return false;
        items[index] = value;
        count++;
        return true;
    }

    //TODO: we need a function for checking if a value exists
    T& operator[](uint32_t i) { return items[i]; }
    const T& operator[](uint32_t i) const { return items[i]; }
    uint32_t size() const { return count; }
    uint32_t max_size() const { return capacity; }

    T* items;

    //TODO: we could make arrays expandable as linked lists of various arrays with a fixed capacity. Essentially once you reach capacity, you allocate (and point to) another array which can be accessed sequentially
private:
    uint32_t count;
    uint32_t capacity;
};
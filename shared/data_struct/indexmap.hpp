#pragma once

#include "types.h"

#include "std/allocator.hpp"
#include "syscalls/syscalls.h"
#include "std/memory.h"

template<typename T>
class IndexMap {
public:

    IndexMap() : items(0), count(0), capacity(0) {
    }

    IndexMap(const IndexMap&) = delete;

    IndexMap<T>* operator=(const IndexMap<T>& orig){
        this->count = orig.count;
        this->capacity = orig.capacity;
        void *mem = (void*)malloc(sizeof(T) * (capacity + 1));
        items = reinterpret_cast<T*>(mem);
        memcpy(this->items, orig.items, sizeof(T) * (capacity + 1));
        default_val = &items[capacity];
        return this;
    }

    IndexMap(uint32_t capacity) : count(0), capacity(capacity) {
        if (capacity == 0) {
            items = 0;
            return;
        }
        void *mem = (void*)malloc(sizeof(T) * (capacity + 1));
        items = reinterpret_cast<T*>(mem);
        default_val = &items[capacity];
    }

    ~IndexMap() {
        if (capacity == 0) return;
        for (uint32_t i = 0; i <= capacity; i++)
            items[i].~T();
        if (items) free_sized(items, sizeof(T) * (capacity + 1));
    }

    bool add(const uint32_t index, const T& value) {
        if (count >= capacity || index >= capacity) return false;
        items[index] = value;
        count++;
        return true;
    }

    T& operator[](uint32_t i) { 
        if (i >= capacity) return *default_val; 
        return items[i]; 
    }
    const T& operator[](uint32_t i) const { 
        if (i >= capacity) return *default_val; 
        return items[i]; 
    }
    uint32_t size() const { return count; }
    uint32_t max_size() const { return capacity; }

    T* items;
    T* default_val;

private:
    uint32_t count;
    uint32_t capacity;
};
//TEST: when assigning an indexmap (like in xhci's endpoint_map), it gets copied. Make sure the old one gets freed

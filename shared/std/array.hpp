#pragma once

#include "types.h"

#include "allocator.hpp"

template<typename T>
class Array {
public:
    static Array create(uint32_t capacity) {
        void* mem = ::operator new(sizeof(T) * capacity);
        Array<T> array;
        array.count = 0;
        array.capacity = capacity;
        array.items = reinterpret_cast<T*>(mem);
        return array;
    }

    void destroy() {
        for (uint32_t i = 0; i < count; i++)
            items[i].~T();
        ::operator delete(this, sizeof(Array) + sizeof(T) * capacity);
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

private:
    uint32_t count;
    uint32_t capacity;
    T* items;
};
#pragma once
#include "types.h"
#include "std/string.h"
#include "std/memfunctions.h"
#include "queue.h"

template<typename T>
class Queue {
public:
    explicit Queue(uint64_t capacity = 0) {
        max_capacity = capacity;
        this->capacity = capacity;
        if (capacity > 0) {
            uintptr_t mem = malloc(capacity * sizeof(T));
            if (mem) {
                buffer = reinterpret_cast<T*>(mem);
            }
        }
    }

    ~Queue() {
        if (buffer) {
            free(buffer, capacity * sizeof(T));
        }
    }

    bool enqueue(const T& value) {
        if (max_capacity > 0 && length == capacity) return false;
        if (max_capacity == 0 && length == capacity) grow_if_needed();
        if (!buffer) return false;

        buffer[head] = value;
        head = (head + 1) % capacity;
        ++length;
        return true;
    }

    bool dequeue(T& out) {
        if (length == 0 || !buffer) return false;
        out = buffer[tail];
        tail = (tail + 1) % capacity;
        --length;
        return true;
    }

    bool is_empty() const {
        return length == 0;
    }

    uint64_t size() const {
        return length;
    }

    void clear() {
        head = tail = length = 0;
    }

private:
    T* buffer = nullptr;
    uint64_t capacity = 0;
    uint64_t max_capacity = 0; // 0 infinite
    uint64_t head = 0;
    uint64_t tail = 0;
    uint64_t length = 0;

    void grow_if_needed() {
        uint64_t new_cap = (capacity > 0) ? capacity * 2 : 4;
        uintptr_t new_mem = malloc(new_cap * sizeof(T));
        if (!new_mem) return;

        T* new_buf = reinterpret_cast<T*>(new_mem);
        for (uint64_t i = 0; i < length; ++i) {
            uint64_t idx = (tail + i) % capacity;
            new_buf[i] = buffer[idx];
        }

        if (buffer) {
            free(buffer, capacity * sizeof(T));
        }

        buffer = new_buf;
        capacity = new_cap;
        tail = 0;
        head = length;
    }
};


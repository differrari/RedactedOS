#pragma once
#include "types.h"
#include "std/memory.h"

//TODO: review allocs & C
template<typename T, uint64_t Capacity>
class RingBuffer {
private:
    T data[Capacity];
    uint64_t head = 0;
    uint64_t tail = 0;
    int32_t full = 0;

public:
    RingBuffer() : head(0), tail(0), full(0) {}

    uint64_t push_buf(const T* src, uint64_t n) {
        if (!src || !n) return 0;
        uint64_t used = size();
        if (used >= Capacity) return 0;
        uint64_t avail = Capacity - used;
        uint64_t to = (n < avail) ? n : avail;
        uint64_t cont = 0;

        if (!full && head < tail) cont = tail - head;
        else cont = Capacity - head;

        uint64_t first = (to < cont) ? to : cont;
        memcpy(data + head, src, first * sizeof(T));
        head = (head + first) % Capacity;

        uint64_t rem = to - first;
        if (rem) {
            memcpy(data + head, src + first, rem * sizeof(T));
            head = (head + rem) % Capacity;
        }

        full = (head == tail);
        return to;
    }

    uint64_t pop_buf(T* dst, uint64_t n) {
        if (!dst || !n) return 0;
        uint64_t used = size();
        if (!used) return 0;
        uint64_t to = (n < used) ? n : used;
        uint64_t cont = 0;

        if (full || tail >= head) cont = Capacity - tail;
        else cont = head - tail;

        uint64_t first = (to < cont) ? to : cont;
        memcpy(dst, data + tail, first * sizeof(T));
        tail = (tail + first) % Capacity;
        full = 0;

        uint64_t rem = to - first;
        if (rem) {
            memcpy(dst + first, data + tail, rem * sizeof(T));
            tail = (tail + rem) % Capacity;
        }

        return to;
    }

    int32_t push(const T& item) {
        if (full) return 0;
        data[head] = item;
        head = (head + 1) % Capacity;
        full = (head == tail);
        return 1;
    }

    int32_t pop(T& out) {
        if (is_empty()) return 0;
        out = data[tail];
        tail = (tail + 1) % Capacity;
        full = 0;
        return 1;
    }

    int32_t is_empty() const {
        return (!full && head == tail);
    }

    int32_t is_full() const {
        return full;
    }

    void clear() {
        head = tail = 0;
        full = 0;
    }

    uint64_t size() const {
        if (full) return Capacity;
        if (head >= tail) return head - tail;
        return Capacity + head - tail;
    }

    static constexpr uint64_t capacity() {
        return Capacity;
    }

    const T& peek() const {
        return data[tail];
    }

    const T& at(uint32_t index) const {
        return data[(tail + index) % Capacity];
    }

    T& at(uint32_t index) {
        return data[(tail + index) % Capacity];
    }
};


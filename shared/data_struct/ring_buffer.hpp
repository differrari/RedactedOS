#pragma once
#include "types.h"

extern "C" {
struct CRingBuffer {
    void* buffer;
    uint64_t capacity; //TODO: define size_t as the same size of os architecture
    uint64_t element_size;
    uint64_t head;
    uint64_t tail;
    int32_t full;
};

void cring_init(struct CRingBuffer* rb, void* storage, uint64_t capacity, uint64_t elem_size);
int32_t cring_push(struct CRingBuffer* rb, const void* item);
int32_t cring_pop(struct CRingBuffer* rb, void* out);
int32_t cring_is_empty(const struct CRingBuffer* rb);
int32_t cring_is_full(const struct CRingBuffer* rb);
void cring_clear(struct CRingBuffer* rb);
uint64_t cring_capacity(const struct CRingBuffer* rb);

}

template<typename T, uint64_t Capacity>
class RingBuffer{

private:
    T data[Capacity];
    uint64_t head = 0;
    uint64_t tail = 0;
    int32_t full = 0;

public:
    int32_t push(const T& item){
        if (full) return 0;
        
        data[head] = item;
        head = (head + 1) % Capacity;
        full = (head == tail);
        return 1;
    }

    int32_t pop(T& out){
        if (is_empty()) return 0;
        
        out = data[tail];
        tail = (tail + 1)% Capacity;
        full = 0;
        return 1;
    }

    int32_t is_empty() const{
        return (!full && (head == tail));
    }

    int32_t is_full() const{
        return full;
    }

    void clear(){
        head = tail = 0;
        full = 0;
    }

    uint64_t size() const{
        if (full) return Capacity;
        
        if (head >= tail) return head - tail;
        
        return Capacity + head - tail;
    }

    const T& peek() const{
        return data[tail];
    }

    const T& at(uint32_t index) const{
        return data[(tail + index) % Capacity];
    }

    T& at(uint32_t index){
        return data[(tail + index)% Capacity];
    }

    static constexpr uint64_t capacity(){ return Capacity; }
};

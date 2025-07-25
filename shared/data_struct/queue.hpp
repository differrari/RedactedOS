#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CQueue {
    void*    buffer;
    uint64_t capacity;      //curremt queue size
    uint64_t max_capacity;  //0 is infinite
    uint64_t elem_size;
    uint64_t head;
    uint64_t tail;
    uint64_t length;
}CQueue;


extern uintptr_t malloc(uint64_t);
extern void free(void*,uint64_t);


void cqueue_init(CQueue* q,uint64_t max_capacity,uint64_t elem_size);
int32_t cqueue_enqueue(CQueue* q,const void* item);
int32_t cqueue_dequeue(CQueue* q,void* out);
int32_t cqueue_is_empty(const CQueue* q);
uint64_t cqueue_size(const CQueue* q);
void cqueue_clear(CQueue* q);
void cqueue_destroy(CQueue* q);

#ifdef __cplusplus
}

template<typename T>
class Queue {
    CQueue q_;

public:
    explicit Queue(uint64_t cap=0){cqueue_init(&q_,cap,sizeof(T));}
    ~Queue(){ cqueue_destroy(&q_); }
    bool enqueue(const T& v){return cqueue_enqueue(&q_,&v);}
    bool dequeue(T& out){return cqueue_dequeue(&q_,&out);}
    bool isEmpty() const{return cqueue_is_empty(&q_);}
    uint64_t size() const{return cqueue_size(&q_);}
    void clear(){cqueue_clear(&q_);}
};

#endif

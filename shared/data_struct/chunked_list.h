#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cchunked_node {
    uint64_t count;
    struct cchunked_node* next;
    void* data[];
} cchunked_node_t;

typedef struct cchunked_list {
    uint64_t chunkSize;
    uint64_t length;
    cchunked_node_t* head;
    cchunked_node_t* tail;
} cchunked_list_t;

uintptr_t malloc(uint64_t size);
void free(void* ptr, uint64_t size);

cchunked_list_t* cchunked_list_create(uint64_t chunkSize);
void cchunked_list_destroy(cchunked_list_t* list);
cchunked_list_t* cchunked_list_clone(const cchunked_list_t* list);
void cchunked_list_push_back(cchunked_list_t* list, void* data);
void* cchunked_list_pop_front(cchunked_list_t* list);
cchunked_node_t* cchunked_list_insert_after(cchunked_list_t* list, cchunked_node_t* node, void* data);
void* cchunked_list_remove_node(cchunked_list_t* list, cchunked_node_t* node);
void cchunked_list_update(cchunked_list_t* list, cchunked_node_t* node, void* new_data);
void cchunked_list_update_at(cchunked_list_t* list, cchunked_node_t* node, uint64_t offset, void* new_data);
uint64_t cchunked_list_length(const cchunked_list_t* list);
uint64_t cchunked_list_size_bytes(const cchunked_list_t* list);
void cchunked_list_for_each(const cchunked_list_t* list, void (*func)(void*));
int cchunked_list_is_empty(const cchunked_list_t* list);

#ifdef __cplusplus
}
#endif

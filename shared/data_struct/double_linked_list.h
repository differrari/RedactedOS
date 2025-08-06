#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdouble_linked_list_node {
    void* data;
    struct cdouble_linked_list_node* next;
    struct cdouble_linked_list_node* prev;
} cdouble_linked_list_node_t;

typedef struct cdouble_linked_list {
    cdouble_linked_list_node_t* head;
    cdouble_linked_list_node_t* tail;
    uint64_t length;
} cdouble_linked_list_t;

extern uintptr_t malloc(uint64_t size);
extern void free(void* ptr, uint64_t size);

cdouble_linked_list_t* cdouble_linked_list_create();
void cdouble_linked_list_destroy(cdouble_linked_list_t* list);
cdouble_linked_list_t* cdouble_linked_list_clone(const cdouble_linked_list_t* list);
void cdouble_linked_list_push_front(cdouble_linked_list_t* list, void* data);
void cdouble_linked_list_push_back(cdouble_linked_list_t* list, void* data);
void* cdouble_linked_list_pop_front(cdouble_linked_list_t* list);
void* cdouble_linked_list_pop_back(cdouble_linked_list_t* list);
cdouble_linked_list_node_t* cdouble_linked_list_insert_after(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node, void* data);
cdouble_linked_list_node_t* cdouble_linked_list_insert_before(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node, void* data);
void* cdouble_linked_list_remove(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node);
void cdouble_linked_list_update(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node, void* new_data);
uint64_t cdouble_linked_list_length(const cdouble_linked_list_t* list);
uint64_t cdouble_linked_list_size_bytes(const cdouble_linked_list_t* list);
cdouble_linked_list_node_t* cdouble_linked_list_find(cdouble_linked_list_t* list, void* key, int (*cmp)(void*, void*));

#ifdef __cplusplus
}
#endif

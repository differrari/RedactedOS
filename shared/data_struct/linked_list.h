#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clinkedlist_node {
    void *data;
    struct clinkedlist_node *next;
} clinkedlist_node_t;

typedef struct clinkedlist {
    clinkedlist_node_t *head;
    clinkedlist_node_t *tail;
    uint64_t length;
} clinkedlist_t;

extern uintptr_t malloc(uint64_t size);
extern void free(void *ptr, uint64_t size);

clinkedlist_t *clinkedlist_create();
void clinkedlist_destroy(clinkedlist_t *list);
clinkedlist_t *clinkedlist_clone(const clinkedlist_t *list);
void clinkedlist_push_front(clinkedlist_t *list, void *data);
void *clinkedlist_pop_front(clinkedlist_t *list);
clinkedlist_node_t *clinkedlist_insert_after(clinkedlist_t *list, clinkedlist_node_t *node, void *data);
void *clinkedlist_remove(clinkedlist_t *list, clinkedlist_node_t *node);
void clinkedlist_update(clinkedlist_t *list, clinkedlist_node_t *node, void *new_data);
uint64_t clinkedlist_length(const clinkedlist_t *list);
uint64_t clinkedlist_size_bytes(const clinkedlist_t *list);
clinkedlist_node_t *clinkedlist_find(clinkedlist_t *list, void *key, int (*cmp)(void *, void *));
void clinkedlist_for_each(const clinkedlist_t *list, void (*func)(void *));

#ifdef __cplusplus
}
#endif

#include "p_queue.h"
#include "syscalls/syscalls.h"

bool p_queue_insert(p_queue_t *root, void* ptr, uint64_t value){
    if (root->size == root->max_size) return false;
    root->array[root->size] = (p_queue_item){ptr, value};
    if (value < root->max_priority){
        root->max_priority = value;
        root->max_priority_index = root->size;
    }
    root->size++;
    return true;
}

int p_queue_peek(p_queue_t*root){
    uint64_t max_priority;
    int index = 0;
    for (int i = 0; i < root->size; i++){
        if (root->array[i].val < max_priority){
            index = i;
            max_priority = root->array[i].val;
        }
    }
    return index;
}

void* p_queue_pop(p_queue_t *root){
    int index = root->max_priority_index;
    void *item = root->array[index].ptr;
    root->max_priority = UINT64_MAX;
    for (int i = 0; i < root->size-1; i++){
        if (i >= index) root->array[i] = root->array[i+1];
        if (root->array[i].val < root->max_priority){
            root->max_priority_index = i;
            root->max_priority = root->array[i].val;
        }
    }
    root->size--;
    return item;
}

void p_queue_traverse(p_queue_t *root){
    for (int i = 0; i < root->size; i++){
        printf("[%i] = %i",i, root->array[i].val);
    }
}

p_queue_t* p_queue_create(int max){
     p_queue_t *root = malloc(sizeof(p_queue_t) + (sizeof(p_queue_item) * max));
     root->max_priority = UINT64_MAX; 
     root->max_size = max;
     root->array = (p_queue_item*)((uintptr_t)root + sizeof(p_queue_t));
     return root;
}

void p_queue_free(p_queue_t *root){
    free_sized(root, sizeof(p_queue_t) + (sizeof(p_queue_item) * root->max_size));
}
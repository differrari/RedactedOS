#include "linked_list.h"

clinkedlist_t *clinkedlist_create(){
    uintptr_t mem = malloc(sizeof(clinkedlist_t));
    if((void *)mem == NULL) return NULL;
    clinkedlist_t *list = (clinkedlist_t *)mem;
    list->head = NULL;
    list->tail = NULL;
    list->length = 0;
    return list;
}

void clinkedlist_destroy(clinkedlist_t *list){
    if(list == NULL) return;
    clinkedlist_node_t *node = list->head;
    while(node){
        clinkedlist_node_t *next = node->next;
        free(node, sizeof(clinkedlist_node_t));
        node = next;
    }
    free(list, sizeof(clinkedlist_t));
}

clinkedlist_t *clinkedlist_clone(const clinkedlist_t *list){
    if(list == NULL) return NULL;
    clinkedlist_t *clone = clinkedlist_create();
    if(clone == NULL) return NULL;
    clinkedlist_node_t *it = list->head;
    while(it){
        if(clone->tail){
            clinkedlist_node_t *new_node = (clinkedlist_node_t *)malloc(sizeof(clinkedlist_node_t));
            new_node->data = it->data;
            new_node->next = NULL;
            clone->tail->next = new_node;
            clone->tail = new_node;
            clone->length++;
        } else {
            clinkedlist_push_front(clone, it->data);
        }
        it = it->next;
    }
    return clone;
}

void clinkedlist_push_front(clinkedlist_t *list, void *data){
    if(list == NULL) return;
    clinkedlist_node_t *node = (clinkedlist_node_t *)malloc(sizeof(clinkedlist_node_t));
    node->data = data;
    node->next = list->head;
    list->head = node;
    if(list->tail == NULL) list->tail = node;
    list->length++;
}

void *clinkedlist_pop_front(clinkedlist_t *list){
    if(list == NULL || list->head == NULL) return NULL;
    clinkedlist_node_t *node = list->head;
    void *data = node->data;
    list->head = node->next;
    if (list->head == NULL) list->tail = NULL;
    list->length--;
    free(node, sizeof(clinkedlist_node_t));
    return data;
}

clinkedlist_node_t *clinkedlist_insert_after(clinkedlist_t *list, clinkedlist_node_t *node, void *data){
    if(list == NULL) return NULL;
    if(node == NULL){
        clinkedlist_push_front(list, data);
        return list->head;
    }
    clinkedlist_node_t *new_node = (clinkedlist_node_t *)malloc(sizeof(clinkedlist_node_t));
    new_node->data = data;
    new_node->next = node->next;
    node->next = new_node;
    if(list->tail == node) list->tail = new_node;
    list->length++;
    return new_node;
}

void *clinkedlist_remove(clinkedlist_t *list, clinkedlist_node_t *node){
    if(list == NULL || node == NULL || list->head == NULL) return NULL;
    if(node == list->head){
        return clinkedlist_pop_front(list);
    }
    clinkedlist_node_t *prev = list->head;
    while(prev->next && prev->next != node){
        prev = prev->next;
    }
    if(prev->next != node) return NULL;
    prev->next = node->next;
    if(node == list->tail) list->tail = prev;
    void *data = node->data;
    list->length--;
    free(node, sizeof(clinkedlist_node_t));
    return data;
}

void clinkedlist_update(clinkedlist_t *list, clinkedlist_node_t *node, void *new_data){
    (void)list;
    if(node) node->data = new_data;
}

uint64_t clinkedlist_length(const clinkedlist_t *list){
    return list ? list->length : 0;
}

uint64_t clinkedlist_size_bytes(const clinkedlist_t *list){
    return list ? list->length * sizeof(clinkedlist_node_t) : 0;
}

clinkedlist_node_t *clinkedlist_find(clinkedlist_t *list, void *key, int (*cmp)(void *, void *)){
    if(list == NULL || cmp == NULL) return NULL;
    clinkedlist_node_t *it = list->head;
    while(it){
        if(cmp(it->data, key) == 0) return it;
        it = it->next;
    }
    return NULL;
}

void clinkedlist_for_each(const clinkedlist_t *list, void (*func)(void *)){
    if(list == NULL || func == NULL) return;
    clinkedlist_node_t *it = list->head;
    while(it){
        func(it->data);
        it = it->next;
    }
}

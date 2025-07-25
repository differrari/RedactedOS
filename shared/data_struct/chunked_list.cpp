#include "chunked_list.hpp"
#include "console/kio.h"

extern "C" {

cchunked_list_t* cchunked_list_create(uint64_t chunkSize){
    uintptr_t raw = malloc(sizeof(cchunked_node_t)+chunkSize*sizeof(void*));
    if (!raw) return NULL;
    
    cchunked_list_t* list = (cchunked_list_t*)malloc(sizeof(cchunked_list_t));
    if (!list){
        free((void*)raw,sizeof(cchunked_node_t)+chunkSize*sizeof(void*));
        return NULL;
    }
    
    list->chunkSize=chunkSize;
    list->length=0;
    list->head=(cchunked_node_t*)raw;
    list->head->count=0;
    list->head->next=NULL;
    list->tail=list->head;
    
    return list;
}

void cchunked_list_destroy(cchunked_list_t* list){
    if (!list) return;
    
    cchunked_node_t* node=list->head;
    while (node){
        cchunked_node_t* next=node->next;
        free(node,sizeof(cchunked_node_t)+list->chunkSize*sizeof(void*));
        node=next;
    }
    
    free(list,sizeof(cchunked_list_t));
}
cchunked_list_t* cchunked_list_clone(const cchunked_list_t* list){
    if (!list) return NULL;
    
    cchunked_list_t* clone=cchunked_list_create(list->chunkSize);
    if (!clone) return NULL;
    
    for (cchunked_node_t* it = list->head; it; it = it->next){
         for (uint64_t i = 0; i<it->count; i++)
            cchunked_list_push_back(clone,it->data[i]);
    }
    return clone;
}

void cchunked_list_push_back(cchunked_list_t* list, void* data){
    if (!list) return;
    //first mnode
    if (list->tail == NULL){
        uintptr_t m = malloc(sizeof(cchunked_node_t) + list->chunkSize * sizeof(void*));
        if (!m) return;
        cchunked_node_t* node = (cchunked_node_t*)m;
        node->count = 0;
        node->next = NULL;
        list->head=list->tail = node;
    }
    
    if (list->tail->count == list->chunkSize){     //last chunk
        uintptr_t m = malloc(sizeof(cchunked_node_t) + list->chunkSize * sizeof(void*));
        if (!m) return;
        cchunked_node_t* node = (cchunked_node_t*)m;
        node->count = 0;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->tail->data[list->tail->count++] = data;
    list->length++;
}

cchunked_node_t* cchunked_list_insert_after(cchunked_list_t* list,cchunked_node_t* node, void* data){
    if (!list) return NULL;
    
    if (!node){
        cchunked_list_push_back(list, data);
        return list->tail;
    }
    
    if(node->count < list->chunkSize){//chunk isnt completrly full
        node->data[node->count++] = data;
        list->length++;
        return node;
    }
    
    //new node
    uintptr_t m = malloc(sizeof(cchunked_node_t) + list->chunkSize * sizeof(void*));
    if (!m) return NULL;
    cchunked_node_t* new_node = (cchunked_node_t*)m;
    new_node->count = 1;
    new_node->next = node->next;
    new_node->data[0] = data;

    //linking new node
    node->next = new_node;
    if (list->tail == node) list->tail = new_node;
    
    list->length++;
    return new_node;
}
void* cchunked_list_pop_front(cchunked_list_t* list){
    if (!list || list->length == 0 || list->head == NULL) return NULL;
    
    void* data = list->head->data[0];
    if (list->head->count > 1){
        for (uint64_t i = 1; i < list->head->count; ++i) list->head->data[i-1] = list->head->data[i];
        list->head->count--;
    } else {
        //uniq element
        cchunked_node_t* old =list->head;
        list->head = old->next;
        if(list->head == NULL) list->tail = NULL;
        free(old, sizeof(cchunked_node_t) + list->chunkSize * sizeof(void*));
    }
    
    list->length--;
    return data;
}



void* cchunked_list_remove_node(cchunked_list_t* list, cchunked_node_t* node){
    if (!list || !node || !list->head) return NULL;

    //delegate to pop_front
    if (node == list->head) return cchunked_list_pop_front(list);

    cchunked_node_t* prev = list->head;
    while (prev->next && prev->next != node) prev= prev->next;
    if (prev->next != node) return NULL;

    void* data = node->data[0];
    for (uint64_t i = 1; i < node->count; ++i) node->data[i-1] = node->data[i];
    
    node->count--;
    list->length--;
    if (node->count == 0){
        prev->next = node->next;
        if(list->tail == node) list->tail = prev;
        free(node, sizeof(cchunked_node_t)+list->chunkSize * sizeof(void*));
    }

    return data;
}

void cchunked_list_update(cchunked_list_t* list, cchunked_node_t* node, void* new_data){
    (void)list;
    if (node&&node->count) node->data[0] = new_data;
}

uint64_t cchunked_list_length(const cchunked_list_t* list){
    return list ? list->length : 0;
}

uint64_t cchunked_list_size_bytes(const cchunked_list_t* list){
    if (!list) return 0;
    uint64_t nodes = 0;
    for (cchunked_node_t* it = list->head; it; it = it->next) nodes++;
    
    return nodes * (sizeof(cchunked_node_t) + list->chunkSize * sizeof(void*));
}

void cchunked_list_for_each(const cchunked_list_t* list, void (*func)(void*)){
    if (!list || !func) return;
    for (cchunked_node_t* it = list->head; it; it = it->next)
        for (uint64_t i = 0; i < it->count; ++i)
            func(it->data[i]);
}
void cchunked_list_update_at(cchunked_list_t* list, cchunked_node_t* node,uint64_t offset,void* new_data){
    if(!list || !node || offset >= node->count)return;
    
    node->data[offset] = new_data;
}

int cchunked_list_is_empty(const cchunked_list_t* list){
    return (!list || list->length == 0) ? 1 : 0;
}
}

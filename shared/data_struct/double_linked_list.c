#include "double_linked_list.h"

cdouble_linked_list_t* cdouble_linked_list_create() {
    uintptr_t raw = malloc((uint64_t)sizeof(cdouble_linked_list_t));
    if (!raw) return NULL;
    cdouble_linked_list_t* list = (cdouble_linked_list_t*)raw;
    list->head = list->tail = NULL;
    list->length = 0;
    return list;
}

void cdouble_linked_list_destroy(cdouble_linked_list_t* list) {
    if (!list) return;
    cdouble_linked_list_node_t* node = list->head;
    for (uint64_t i = 0; i < list->length; ++i) {
        cdouble_linked_list_node_t* next = node->next;
        free(node, sizeof(cdouble_linked_list_node_t));
        node = next;
    }
    free(list, sizeof(cdouble_linked_list_t));
}

cdouble_linked_list_t* cdouble_linked_list_clone(const cdouble_linked_list_t* list) {
    if (!list) return NULL;
    cdouble_linked_list_t* clone = cdouble_linked_list_create();
    if (!clone) return NULL;
    cdouble_linked_list_node_t* it = list->head;
    for (uint64_t i = 0; i < list->length; ++i) {
        uintptr_t raw = malloc(sizeof(cdouble_linked_list_node_t));
        if (raw) {
            cdouble_linked_list_node_t* node = (cdouble_linked_list_node_t*)raw;
            node->data = it->data;
            if (clone->length == 0) {
                node->next = node->prev = node;
                clone->head = clone->tail = node;
            } else {
                node->prev = clone->tail;
                node->next = clone->head;
                clone->tail->next = node;
                clone->head->prev = node;
                clone->tail = node;
            }
            ++clone->length;
        }
        it = it->next;
    }
    return clone;
}

void cdouble_linked_list_push_front(cdouble_linked_list_t* list, void* data) {
    if (!list) return;
    uintptr_t raw = malloc(sizeof(cdouble_linked_list_node_t));
    if (!raw) return;
    cdouble_linked_list_node_t* node = (cdouble_linked_list_node_t*)raw;
    node->data = data;
    if (list->length == 0) {
        node->next = node->prev = node;
        list->head = list->tail = node;
    } else {
        node->next = list->head;
        node->prev = list->tail;
        list->head->prev = node;
        list->tail->next = node;
        list->head = node;
    }
    ++list->length;
}

void cdouble_linked_list_push_back(cdouble_linked_list_t* list, void* data) {
    if (!list) return;
    uintptr_t raw = malloc(sizeof(cdouble_linked_list_node_t));
    if (!raw) return;
    cdouble_linked_list_node_t* node = (cdouble_linked_list_node_t*)raw;
    node->data = data;
    if (list->length == 0) {
        node->next = node->prev = node;
        list->head = list->tail = node;
    } else {
        node->prev = list->tail;
        node->next = list->head;
        list->tail->next = node;
        list->head->prev = node;
        list->tail = node;
    }
    ++list->length;
}

void* cdouble_linked_list_pop_front(cdouble_linked_list_t* list) {
    if (!list || list->length == 0) return NULL;
    cdouble_linked_list_node_t* node = list->head;
    void* data = node->data;
    if (list->length == 1) {
        list->head = list->tail = NULL;
    } else {
        list->head = node->next;
        list->head->prev = list->tail;
        list->tail->next = list->head;
    }
    --list->length;
    free(node, sizeof(cdouble_linked_list_node_t));
    return data;
}

void* cdouble_linked_list_pop_back(cdouble_linked_list_t* list) {
    if (!list || list->length == 0) return NULL;
    cdouble_linked_list_node_t* node = list->tail;
    void* data = node->data;
    if (list->length == 1) {
        list->head = list->tail = NULL;
    } else {
        list->tail = node->prev;
        list->tail->next = list->head;
        list->head->prev = list->tail;
    }
    --list->length;
    free(node, sizeof(cdouble_linked_list_node_t));
    return data;
}

cdouble_linked_list_node_t* cdouble_linked_list_insert_after(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node, void* data) {
    if (!list) return NULL;
    if (!node) {
        cdouble_linked_list_push_front(list, data);
        return list->head;
    }
    uintptr_t raw = malloc(sizeof(cdouble_linked_list_node_t));
    if (!raw) return NULL;
    cdouble_linked_list_node_t* new_node = (cdouble_linked_list_node_t*)raw;
    new_node->data = data;
    new_node->next = node->next;
    new_node->prev = node;
    node->next->prev = new_node;
    node->next = new_node;
    if (list->tail == node) list->tail = new_node;
    ++list->length;
    return new_node;
}

cdouble_linked_list_node_t* cdouble_linked_list_insert_before(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node, void* data) {
    if (!list) return NULL;
    if (!node) {
        cdouble_linked_list_push_back(list, data);
        return list->tail;
    }
    return cdouble_linked_list_insert_after(list, node->prev, data);
}

void* cdouble_linked_list_remove(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node) {
    if (!list || !node || list->length == 0) return NULL;
    if (node == list->head) return cdouble_linked_list_pop_front(list);
    if (node == list->tail) return cdouble_linked_list_pop_back(list);
    node->prev->next = node->next;
    node->next->prev = node->prev;
    void* data = node->data;
    --list->length;
    free(node, sizeof(cdouble_linked_list_node_t));
    return data;
}

void cdouble_linked_list_update(cdouble_linked_list_t* list, cdouble_linked_list_node_t* node, void* new_data) {
    (void)list;
    if (node) node->data = new_data;
}

uint64_t cdouble_linked_list_length(const cdouble_linked_list_t* list) {
    return list ? list->length : 0;
}

uint64_t cdouble_linked_list_size_bytes(const cdouble_linked_list_t* list) {
    return list ? list->length * sizeof(cdouble_linked_list_node_t) : 0;
}

cdouble_linked_list_node_t* cdouble_linked_list_find(cdouble_linked_list_t* list, void* key, int (*cmp)(void*, void*)) {
    if (!list || !cmp || list->length == 0) return NULL;
    cdouble_linked_list_node_t* it = list->head;
    for (uint64_t i = 0; i < list->length; ++i) {
        if (cmp(it->data, key) == 0) return it;
        it = it->next;
    }
    return NULL;
}

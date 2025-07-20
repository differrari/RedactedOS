#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct clinkedlist_node{
    void *data;
    struct clinkedlist_node *next;
}clinkedlist_node_t;

typedef struct clinkedlist{
    clinkedlist_node_t *head;
    clinkedlist_node_t *tail;
    uint64_t length;
}clinkedlist_t;

extern uintptr_t malloc(uint64_t size);
extern void free(void *ptr, uint64_t size);

clinkedlist_t *clinkedlist_create(void);
void clinkedlist_destroy(clinkedlist_t *list);
clinkedlist_t *clinkedlist_clone(const clinkedlist_t *list);
void clinkedlist_push_front(clinkedlist_t *list,void *data);
void *clinkedlist_pop_front(clinkedlist_t *list);
clinkedlist_node_t *clinkedlist_insert_after(clinkedlist_t *list, clinkedlist_node_t *node, void *data);
void *clinkedlist_remove(clinkedlist_t *list, clinkedlist_node_t *node);
void clinkedlist_update(clinkedlist_t *list, clinkedlist_node_t *node, void *new_data);
uint64_t clinkedlist_length(const clinkedlist_t *list);
uint64_t clinkedlist_size_bytes(const clinkedlist_t *list);
clinkedlist_node_t *clinkedlist_find(clinkedlist_t *list, void *key,int (*cmp)(void *, void *));
void clinkedlist_for_each(const clinkedlist_t *list, void (*func)(void *));

#ifdef __cplusplus
}

template <typename T> class LinkedList{
private:
    struct Node{
        T data;
        Node *next;
    };
    Node *head = nullptr;
    Node *tail = nullptr;
    uint64_t length = 0;

    Node *alloc_node(const T &value){
        uintptr_t mem = malloc(sizeof(Node));
        Node *n = reinterpret_cast<Node *>(mem);
        n->data = value;
        n->next = nullptr;
        return n;
    }
    void free_node(Node *n){
        free(n, sizeof(Node));
    }

public:
    LinkedList() = default;

    LinkedList(const LinkedList<T> &other){
        for(Node *it = other.head; it; it = it->next){
            push_front(it->data);
        }
        LinkedList<T> tmp;
        while(!empty()){
            tmp.push_front(pop_front());
        }
        *this = tmp;
    }

    ~LinkedList(){
        while(!empty()) pop_front();
    }

    LinkedList<T> &operator=(const LinkedList<T> &other){
        if(this != &other){
            while(!empty()) pop_front();
            for(Node *it = other.head; it; it = it->next){
                push_front(it->data);
            }
            LinkedList<T> tmp;
            while(!empty()){
                tmp.push_front(pop_front());
            }
            head = tmp.head;
            tail = tmp.tail;
            length = tmp.length;
            tmp.head = tmp.tail = nullptr;
            tmp.length = 0;
        }
        return *this;
    }

    void push_front(const T &value){
        Node *n = alloc_node(value);
        n->next = head;
        head = n;
        if (tail == nullptr) tail = n;
        ++length;
    }

    T pop_front(){
        Node *n = head;
        head = head->next;
        if (head == nullptr) tail = nullptr;
        T val = n->data;
        free_node(n);
        --length;
        return val;
    }

    Node *insert_after(Node *node, const T &value){
        if(node == nullptr){
            push_front(value);
            return head;
        }
        Node *n = alloc_node(value);
        n->next = node->next;
        node->next = n;
        if(tail == node) tail = n;
        ++length;
        return n;
    }

    T remove(Node *node){
        if(node == nullptr) return T();
        if(node == head) return pop_front();
        Node *prev = head;
        while(prev && prev->next != node) prev = prev->next;
        if(prev == nullptr) return T();
        prev->next = node->next;
        if(node == tail) tail = prev;
        T val = node->data;
        free_node(node);
        --length;
        return val;
    }

    void update(Node *node, const T &value){
        if(node) node->data = value;
    }

    uint64_t size() const{return length;}
    bool empty() const{return length == 0; }
    Node *begin() const{ return head; }
    Node *end() const{ return nullptr;}
    template <typename Predicate>
    Node *find(Predicate pred) const{
        for(Node *it = head; it; it = it->next){
            if(pred(it->data)) return it;
        }
        return nullptr;
    }
};

#endif

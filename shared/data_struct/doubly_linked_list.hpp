#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdouble_linked_list_node{
    void *data;
    struct cdouble_linked_list_node *next;
    struct cdouble_linked_list_node *prev;
}cdouble_linked_list_node_t;

typedef struct cdouble_linked_list{
    cdouble_linked_list_node_t *head;
    cdouble_linked_list_node_t *tail;
    uint64_t length;
}cdouble_linked_list_t;

extern uintptr_t malloc(uint64_t size);
extern void free(void *ptr, uint64_t size);

cdouble_linked_list_t *cdouble_linked_list_create(void);
void cdouble_linked_list_destroy(cdouble_linked_list_t *list);
cdouble_linked_list_t *cdouble_linked_list_clone(const cdouble_linked_list_t *list);
void cdouble_linked_list_push_front(cdouble_linked_list_t *list, void *data);
void cdouble_linked_list_push_back(cdouble_linked_list_t *list, void *data);
void *cdouble_linked_list_pop_front(cdouble_linked_list_t *list);
void *cdouble_linked_list_pop_back(cdouble_linked_list_t *list);
cdouble_linked_list_node_t *cdouble_linked_list_insert_after(cdouble_linked_list_t *list, cdouble_linked_list_node_t *node, void *data);
cdouble_linked_list_node_t *cdouble_linked_list_insert_before(cdouble_linked_list_t *list, cdouble_linked_list_node_t *node, void *data);
void *cdouble_linked_list_remove(cdouble_linked_list_t *list, cdouble_linked_list_node_t *node);
void cdouble_linked_list_update(cdouble_linked_list_t *list, cdouble_linked_list_node_t *node, void *new_data);
uint64_t cdouble_linked_list_length(const cdouble_linked_list_t *list);
uint64_t cdouble_linked_list_size_bytes(const cdouble_linked_list_t *list);
cdouble_linked_list_node_t* cdouble_linked_list_find(cdouble_linked_list_t *list, void *key, int (*cmp)(void *, void *));
#ifdef __cplusplus
}

template<typename T>
class LinkedList{
private:
    struct Node{
        T data;
        Node *next;
        Node *prev;
    };

    Node *head;
    Node *tail;
    uint64_t length;

    Node* alloc_node(const T& value){
        uintptr_t raw= malloc((uint64_t)sizeof(Node));
        if(raw == 0) return nullptr;
        Node* n =(Node*)raw;
        n->data=value;
        n->next=n->prev=nullptr;
        return n;
    }
    void free_node(Node* n){
        if(!n) return;
        free(n,(uint64_t)sizeof(Node));
    }

    static void swap(LinkedList& a, LinkedList& b) noexcept{
        Node* tmp_head = a.head;
        a.head= b.head;
        b.head= tmp_head;
        Node* tmp_tail =a.tail;
        a.tail= b.tail;
        b.tail= tmp_tail;
        uint64_t tmp_length = a.length;
        a.length = b.length;
        b.length = tmp_length;
    }

public:
    LinkedList():head(nullptr), tail(nullptr), length(0){}

    LinkedList(const LinkedList& other):head(nullptr), tail(nullptr), length(0){
        if(other.head){
            Node* it = other.head;
            do{
                push_back(it->data);
                it = it->next;
            }while(it != other.head);
        }
    }

    ~LinkedList(){
        while(!empty()) pop_front();
    }

    LinkedList& operator=(const LinkedList& other){
        if(this != &other){
            LinkedList tmp(other);
            swap(*this, tmp);
        }
        return *this;
    }

    void push_front(const T& value){
        Node* n = alloc_node(value);
        if(!n) return;
        if(!head){
            head = tail = n;
            n->next = n->prev = n;
        }else{
            n->next= head;
            n->prev= tail;
            head->prev=n;
            tail->next= n;
            head= n;
        }
        ++length;
    }

    void push_back(const T& value){
        Node* n =alloc_node(value);
        if(!n) return;
        if(!tail){
            head = tail = n;
            n->next = n->prev = n;
        }else{
            n->prev      = tail;
            n->next      = head;
            tail->next   = n;
            head->prev   = n;
            tail         = n;
        }
        ++length;
    }

    T pop_front(){
        if(!head) return T();
        Node* n = head;
        T val = n->data;
        if(head == tail){
            head =tail=nullptr;
        }else{
            head = head->next;
            head->prev = tail;
            tail->next = head;
        }
        --length;
        free_node(n);
        return val;
    }

    T pop_back(){
        if(!tail) return T();
        Node* n = tail;
        T val = n->data;
        if(head == tail){
            head = tail = nullptr;
        }else{
            tail = tail->prev;
            tail->next = head;
            head->prev = tail;
        }
        --length;
        free_node(n);
        return val;
    }

    Node* insert_after(Node* node, const T& value){
        if(!node){
            push_front(value);
            return head;
        }
        Node* n= alloc_node(value);
        if(!n) return nullptr;
        n->next=node->next;
        n->prev=node;
        node->next->prev = n;
        node->next= n;
        if(tail==node) tail = n;
        ++length;
        return n;
    }

    Node* insert_before(Node* node, const T& value){
        if(!node){
            push_back(value);
            return tail;
        }
        return insert_after(node->prev, value);
    }

    T remove(Node* node){
        if(!node) return T();
        if(node == head) return pop_front();
        if(node == tail) return pop_back();
        node->prev->next=node->next;
        node->next->prev=node->prev;
        T val=node->data;
        --length;
        free_node(node);
        return val;
    }

    void update(Node* node, const T& value){
        if(!node) return;
        node->data = value;
    }

    uint64_t size()const{return length;}
    bool empty() const{return length==0;}
    Node* begin() const{return head;}
    Node* end() const{return nullptr;}

    template<typename Predicate>
    Node* find(Predicate pred) const{
        if(!head) return nullptr;
        Node* it=head;
        do{
            if(pred(it->data)) return it;
            it=it->next;
        }while(it != head);
        return nullptr;
    }

    template<typename Func>
    void for_each(Func func) const{
        if(!head) return;
        Node* it = head;
        do{
            func(it->data);
            it = it->next;
        }while(it != head);
    }
};
#endif

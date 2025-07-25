#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uintptr_t malloc(uint64_t size);
extern void free(void *ptr, uint64_t size);

typedef struct cchunked_node{
    uint64_t count;
    struct cchunked_node* next;
    void* data[];
}cchunked_node_t;

typedef struct cchunked_list{
    uint64_t chunkSize;
    uint64_t length;
    cchunked_node_t* head;
    cchunked_node_t* tail;
}cchunked_list_t;

cchunked_list_t* cchunked_list_create(uint64_t chunkSize);
void cchunked_list_destroy(cchunked_list_t* list);
cchunked_list_t* cchunked_list_clone(const cchunked_list_t* list);
void cchunked_list_push_back(cchunked_list_t* list, void* data);
void* cchunked_list_pop_front(cchunked_list_t* list);
cchunked_node_t* cchunked_list_insert_after(cchunked_list_t* list, cchunked_node_t* node, void* data);
void* cchunked_list_remove_node(cchunked_list_t* list, cchunked_node_t* node);
void cchunked_list_update(cchunked_list_t* list, cchunked_node_t* node, void* new_data);
void cchunked_list_update_at(cchunked_list_t* list, cchunked_node_t* node,uint64_t offset, void* new_data);
uint64_t cchunked_list_length(const cchunked_list_t* list);
uint64_t cchunked_list_size_bytes(const cchunked_list_t* list);
void cchunked_list_for_each(const cchunked_list_t* list, void (*func)(void*));
int cchunked_list_is_empty(const cchunked_list_t* list);

#ifdef __cplusplus
}
template<typename T>
class ChunkedList{
public:
    struct Node{
        uint64_t count;
        Node* next;
        T data[];
    };

    explicit ChunkedList(uint64_t cs): head(nullptr), tail(nullptr), length_(0), chunkSize(cs){}
    ~ChunkedList(){
        while (head){
            Node* nx = head->next;
            ::free(head, sizeof(Node) + chunkSize * sizeof(T));
            head = nx;
        }
    }

    ChunkedList(const ChunkedList& other): head(nullptr), tail(nullptr), length_(0), chunkSize(other.chunkSize){
        for (Node* c = other.head; c; c = c->next)
            for (uint64_t i = 0; i < c->count; ++i)
                push_back(c->data[i]);
    }

    ChunkedList& operator=(const ChunkedList& other){
        if (this != &other){
            clear();
            chunkSize = other.chunkSize;
            for (Node* c = other.head; c; c = c->next)
                for (uint64_t i = 0; i < c->count; ++i)
                    push_back(c->data[i]);
        }
        return *this;
    }

    void push_back(const T& value){
        if (!tail) allocFirst();
        if (tail->count == chunkSize) allocChunk();
        tail->data[tail->count++] = value;
        ++length_;
    }

    T pop_front(){
        if (!head) return T();
        T val = head->data[0];
        if (head->count > 1){
            for (uint64_t i = 1; i < head->count; ++i) head->data[i-1] = head->data[i];
            --head->count;
        } else {
            Node* old = head;
            head = head->next;
            ::free(old, sizeof(Node) + chunkSize * sizeof(T));
            if (!head) tail = nullptr;
        }
        --length_;
        return val;
    }

    Node* insert_after(Node* node, const T& value){
        if (!node){
            push_back(value);
            return tail;
        }
        if (node->count < chunkSize){
            node->data[node->count++] = value;
            ++length_;
            return node;
        }
        Node* n=allocNode(value);
        n->next = node->next;
        node->next = n;
        if (tail ==node) tail = n;
        ++length_;
        return n;
    }

T remove_node(Node* node){
    if (!node|| !head) return T();
    if (node == head){
        T val = head->data[0];
        uint64_t removed = head->count;
        Node* nxt = head->next;
        ::free(head, sizeof(Node) + chunkSize * sizeof(T));
        head = nxt;
        if(!head) tail = nullptr;
        length_ -= removed;
        return val;
    }
    Node* prev = head;
    while (prev->next && prev->next != node) prev = prev->next;
    if (prev->next != node) return T();
    
    T val = node->data[0];
    uint64_t removed = node->count;
    prev->next = node->next;
    if (tail==node) tail = prev;
    
    ::free(node, sizeof(Node) + chunkSize * sizeof(T));
    length_ -= removed;
    return val;
}

    void update(Node* node, const T& value){
        if (node && node->count) node->data[0] = value;
    }

    void update_at(Node* node, uint64_t offset, const T& value){
        if (node && offset < node->count) node->data[offset] = value;
    }

    uint64_t size() const noexcept{return length_;}

    uint64_t size_bytes() const noexcept{
        uint64_t nodes = 0;
        for (Node* it = head; it; it = it->next) ++nodes;
        return nodes * (sizeof(Node) + chunkSize * sizeof(T));
    }

    bool empty() const noexcept{return length_ == 0; }

    template<typename Func>
    void for_each(Func f) const{
        for (Node* it = head; it; it = it->next)
            for (uint64_t i = 0; i < it->count; ++i)
                f(it->data[i]);
    }

private:
    Node* head = nullptr;
    Node* tail = nullptr;
    uint64_t length_ = 0;
    uint64_t chunkSize;

    Node* allocNode(const T& value){
        uintptr_t raw= ::malloc(sizeof(Node) + chunkSize * sizeof(T));
        Node* n = reinterpret_cast<Node*>(raw);
        n->count = 1;
        n->next = nullptr;
        n->data[0] = value;
        return n;
    }

    void allocFirst(){
        uintptr_t raw = ::malloc(sizeof(Node) +chunkSize*sizeof(T));
        head = tail = reinterpret_cast<Node*>(raw);
        head->count = 0;
        head->next = nullptr;
    }

    void allocChunk(){
        uintptr_t raw = ::malloc(sizeof(Node)+ chunkSize * sizeof(T));
        Node* n=reinterpret_cast<Node*>(raw);
        n->count = 0;
        n->next = nullptr;
        tail->next = n;
        tail = n;
    }

    void clear(){
        while (head) pop_front();
    }
};
#endif

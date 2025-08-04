#pragma once
#include "types.h"

extern "C" {
#include "linked_list.h"
}

template <typename T>
class LinkedList {
private:
    struct Node {
        T data;
        Node* next;
    };

    Node* head = nullptr;
    Node* tail = nullptr;
    uint64_t length = 0;

    Node* alloc_node(const T& value) {
        uintptr_t mem = malloc(sizeof(Node));
        Node* n = reinterpret_cast<Node*>(mem);
        n->data = value;
        n->next = nullptr;
        return n;
    }

    void free_node(Node* n) {
        free(n, sizeof(Node));
    }

public:
    LinkedList() = default;

    LinkedList(const LinkedList<T>& other) {
        for (Node* it = other.head; it; it = it->next) {
            push_front(it->data);
        }
        LinkedList<T> tmp;
        while (!empty()) tmp.push_front(pop_front());
        *this = tmp;
    }

    ~LinkedList() {
        while (!empty()) pop_front();
    }

    LinkedList<T>& operator=(const LinkedList<T>& other) {
        if (this != &other) {
            while (!empty()) pop_front();
            for (Node* it = other.head; it; it = it->next) {
                push_front(it->data);
            }
            LinkedList<T> tmp;
            while (!empty()) tmp.push_front(pop_front());
            head = tmp.head;
            tail = tmp.tail;
            length = tmp.length;
            tmp.head = tmp.tail = nullptr;
            tmp.length = 0;
        }
        return *this;
    }

    void push_front(const T& value) {
        Node* n = alloc_node(value);
        n->next = head;
        head = n;
        if (!tail) tail = n;
        ++length;
    }

    T pop_front() {
        if (!head) return T();
        Node* n = head;
        head = head->next;
        if (!head) tail = nullptr;
        T val = n->data;
        free_node(n);
        --length;
        return val;
    }

    Node* insert_after(Node* node, const T& value) {
        if (!node) {
            push_front(value);
            return head;
        }
        Node* n = alloc_node(value);
        n->next = node->next;
        node->next = n;
        if (tail == node) tail = n;
        ++length;
        return n;
    }

    T remove(Node* node) {
        if (!node) return T();
        if (node == head) return pop_front();
        Node* prev = head;
        while (prev && prev->next != node) prev = prev->next;
        if (!prev) return T();
        prev->next = node->next;
        if (node == tail) tail = prev;
        T val = node->data;
        free_node(node);
        --length;
        return val;
    }

    void update(Node* node, const T& value) {
        if (node) node->data = value;
    }

    uint64_t size() const {
        return length;
    }

    bool empty() const {
        return length == 0;
    }

    Node* begin() const {
        return head;
    }

    Node* end() const {
        return nullptr;
    }

    template <typename Predicate>
    Node* find(Predicate pred) const {
        for (Node* it = head; it; it = it->next) {
            if (pred(it->data)) return it;
        }
        return nullptr;
    }
};


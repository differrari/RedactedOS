#pragma once
#include "types.h"

extern "C" {
#include "double_linked_list.h"
}

template<typename T>
class DoubleLinkedList {
private:
    struct Node {
        T data;
        Node* next;
        Node* prev;
    };

    Node* head;
    Node* tail;
    uint64_t length;

    Node* alloc_node(const T& value) {
        uintptr_t raw = malloc(sizeof(Node));
        if (raw == 0) return nullptr;
        Node* n = reinterpret_cast<Node*>(raw);
        n->data = value;
        n->next = n->prev = nullptr;
        return n;
    }

    void free_node(Node* n) {
        if (!n) return;
        free(n, sizeof(Node));
    }

    static void swap(DoubleLinkedList& a, DoubleLinkedList& b) noexcept {
        Node* tmpHead = a.head;
        a.head = b.head;
        b.head = tmpHead;

        Node* tmpTail = a.tail;
        a.tail = b.tail;
        b.tail = tmpTail;

        size_t tmpLen = a.length;
        a.length = b.length;
        b.length = tmpLen;
    }

public:
    DoubleLinkedList() : head(nullptr), tail(nullptr), length(0) {}

    DoubleLinkedList(const DoubleLinkedList& other) : head(nullptr), tail(nullptr), length(0) {
        if (other.head) {
            Node* it = other.head;
            do {
                push_back(it->data);
                it = it->next;
            } while (it != other.head);
        }
    }

    ~DoubleLinkedList() {
        while (!empty()) pop_front();
    }

    DoubleLinkedList& operator=(const DoubleLinkedList& other) {
        if (this != &other) {
            DoubleLinkedList tmp(other);
            swap(*this, tmp);
        }
        return *this;
    }

    void push_front(const T& value) {
        Node* n = alloc_node(value);
        if (!n) return;
        if (!head) {
            head = tail = n;
            n->next = n->prev = n;
        } else {
            n->next = head;
            n->prev = tail;
            head->prev = n;
            tail->next = n;
            head = n;
        }
        ++length;
    }

    void push_back(const T& value) {
        Node* n = alloc_node(value);
        if (!n) return;
        if (!tail) {
            head = tail = n;
            n->next = n->prev = n;
        } else {
            n->prev = tail;
            n->next = head;
            tail->next = n;
            head->prev = n;
            tail = n;
        }
        ++length;
    }

    T pop_front() {
        if (!head) return T();
        Node* n = head;
        T val = n->data;
        if (head == tail) {
            head = tail = nullptr;
        } else {
            head = head->next;
            head->prev = tail;
            tail->next = head;
        }
        --length;
        free_node(n);
        return val;
    }

    T pop_back() {
        if (!tail) return T();
        Node* n = tail;
        T val = n->data;
        if (head == tail) {
            head = tail = nullptr;
        } else {
            tail = tail->prev;
            tail->next = head;
            head->prev = tail;
        }
        --length;
        free_node(n);
        return val;
    }

    Node* insert_after(Node* node, const T& value) {
        if (!node) {
            push_front(value);
            return head;
        }
        Node* n = alloc_node(value);
        if (!n) return nullptr;
        n->next = node->next;
        n->prev = node;
        node->next->prev = n;
        node->next = n;
        if (tail == node) tail = n;
        ++length;
        return n;
    }

    Node* insert_before(Node* node, const T& value) {
        if (!node) {
            push_back(value);
            return tail;
        }
        return insert_after(node->prev, value);
    }

    T remove(Node* node) {
        if (!node) return T();
        if (node == head) return pop_front();
        if (node == tail) return pop_back();
        node->prev->next = node->next;
        node->next->prev = node->prev;
        T val = node->data;
        --length;
        free_node(node);
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

    template<typename Predicate>
    Node* find(Predicate pred) const {
        if (!head) return nullptr;
        Node* it = head;
        do {
            if (pred(it->data)) return it;
            it = it->next;
        } while (it != head);
        return nullptr;
    }

    template<typename Func>
    void for_each(Func func) const {
        if (!head) return;
        Node* it = head;
        do {
            func(it->data);
            it = it->next;
        } while (it != head);
    }
};


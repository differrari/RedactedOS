#include "huffman.h"
#include "syscalls/syscalls.h"
#include "math/math.h"

int* huff_count_entries(sizedptr input){
    int *entries = malloc(sizeof(int) * 257);
    uint8_t *buf = (uint8_t*)input.ptr;
    for (size_t i = 0; i < input.size; i++){
        entries[buf[i]]++;
    }
    int count = 0;
    for (int i = 0; i < 256; i++){
        if (entries[i]){ count++; printf("%c = %i", i, entries[i]); }
    }
    entries[256] = count;
    printf("%i unique bytes", entries[256]);
    return entries;
}

typedef struct huff_tree_node {
    struct huff_tree_node *left, *right;
    uint8_t entry;
    uint8_t depth;
    char byte;
    int index;
} huff_tree_node;

typedef struct p_queue_item {
    void* ptr;
    uint64_t val;
} p_queue_item;

typedef struct p_queue_t {
    p_queue_item *array;
    int size;
    uint64_t max_priority;
    int max_priority_index;
} p_queue_t;

void p_queue_insert(p_queue_t *root, void* ptr, uint8_t value){
    root->array[root->size] = (p_queue_item){ptr, value};
    if (value < root->max_priority){
        root->max_priority = value;
        root->max_priority_index = root->size;
    }
    root->size++;
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
     p_queue_t *root = malloc(sizeof(p_queue_t));
     root->max_priority = UINT64_MAX; 
     root->array = malloc(sizeof(p_queue_item) * max);
     return root;
}

int node_index = 0;

#define TO_READABLE(c) (c >= 'a' && c <= 'z' ? c : '?')

huff_tree_node* huff_make_node(huff_tree_node *l, huff_tree_node *r){
    huff_tree_node *node = malloc(sizeof(huff_tree_node));//TODO: no need for an alloc every time, we know the max number of nodes
    node->left = l;
    node->right = r;
    node->depth = max(l->depth, r->depth) + 1;
    node->entry = l->entry + r->entry;
    node->index = node_index++;
    printf("%i = %i\n-> l = %i %c - %i\n-> r = %i %c - %i", node->index, node->entry, l->index, TO_READABLE(l->byte), l->entry, r->index, TO_READABLE(r->byte), r->entry);
    return node;
}

void huff_calc_code(huff_tree_node *root, uint8_t value, uint8_t depth){
    bool leaf = true;
    if (root->left){
        leaf = false;
        huff_calc_code(root->left, value << 1, depth+1);
    }
    if (root->right){
        leaf = false;
        value |= 1;
        huff_calc_code(root->right, value << 1, depth+1);
    }
    if (leaf) printf("%c = [%b]. Depth %i",root->byte, value >> 1, depth);
}

huff_tree_node* huff_build_tree(int entries[257]){
    int count = entries[256];
    p_queue_t *heap = p_queue_create((count * 2 + (count % 2)));
    int index = 0;
    for (int i = 0; i < count; i++){
        while (!entries[index]) index++;
        if (index < 256){
            huff_tree_node *node = malloc(sizeof(huff_tree_node));//TODO: no need for an alloc every time, we know the max number of nodes
            node->entry = entries[index];
            node->byte = index;
            node->index = node_index++;
            p_queue_insert(heap, node, entries[index]);
            index++;
        }
    }
    while (heap->size > 1){
        huff_tree_node *l = p_queue_pop(heap);
        huff_tree_node *r = p_queue_pop(heap);
        huff_tree_node *root = huff_make_node(l, r);
        p_queue_insert(heap, root, root->entry);
    }

    huff_tree_node *root = p_queue_pop(heap);
    
    return root;
}

void huffman_encode(sizedptr input, sizedptr output){
    int* entries = huff_count_entries(input);
    huff_tree_node *root = huff_build_tree(entries);
    huff_calc_code(root, 0, 0);
}
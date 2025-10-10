#pragma once

#include "types.h"

/*
    This compression algorithm is (very experimental). 
    It can only encode byte values, and is not optimized in the slightest.
*/

typedef struct huff_tree_node {
    struct huff_tree_node *left, *right;
    uint16_t entry;
    uint8_t depth;//DEBUG
    char byte;//DEBUG
    int index;//DEBUG
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

void huffman_encode(sizedptr input, sizedptr output);
void huffman_populate(huff_tree_node *root, uint64_t code, uint8_t code_len, uint16_t value);
void huffman_viz(huff_tree_node *root, uint8_t depth, uint64_t val);
static inline huff_tree_node *huffman_traverse(huff_tree_node *root, uint8_t bit){
    return (bit & 1) ? root->right : root->left;
}
void huffman_free(huff_tree_node *root);
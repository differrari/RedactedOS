#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

//TODO: review allocs & C
extern void* malloc(size_t size);
extern void free_sized(void* ptr, size_t size);

typedef uint64_t (*chashmap_hash_fn)(const void* key, uint64_t len);
typedef int (*chashmap_keyeq_fn)(const void* a, uint64_t alen, const void* b, uint64_t blen);

typedef struct chashmap_entry {
    uint64_t hash;
    void* key;
    uint64_t key_len;
    void* value;
    struct chashmap_entry* next;
} chashmap_entry_t;

typedef struct chashmap {
    chashmap_entry_t** buckets;
    uint64_t capacity;
    uint64_t size;
    void* (*alloc)(size_t size);
    void (*free)(void* ptr, size_t size);
    chashmap_hash_fn hash_fn;
    chashmap_keyeq_fn keyeq_fn;
    void (*value_dispose)(void* value);
    uint64_t resize_threshold;
} chashmap_t;

chashmap_t* chashmap_create(uint64_t initial_capacity);
chashmap_t* chashmap_create_alloc(uint64_t initial_capacity, void* (*alloc)(size_t size),void (*mfree)(void* ptr, size_t size));
void chashmap_destroy(chashmap_t* map);
void chashmap_set_allocator(chashmap_t* map, void* (*alloc)(size_t), void (*dealloc)(void*, size_t));
void chashmap_set_hash(chashmap_t* map, chashmap_hash_fn hash_fn, chashmap_keyeq_fn keyeq_fn);
void chashmap_set_value_dispose(chashmap_t* map, void (*dispose_fn)(void*));
int chashmap_put(chashmap_t* map, const void* key, uint64_t key_len, void* value);
void* chashmap_get(const chashmap_t* map, const void* key, uint64_t key_len);
int chashmap_remove(chashmap_t* map, const void* key, uint64_t key_len, void** out_value);
uint64_t chashmap_size(const chashmap_t* map);
uint64_t chashmap_capacity(const chashmap_t* map);
void chashmap_for_each(const chashmap_t* map, void (*func)(void* key, uint64_t key_len, void* value));
uint64_t chashmap_fnv1a64(const void* data, uint64_t len);

#ifdef __cplusplus
}
#endif

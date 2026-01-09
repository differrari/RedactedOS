#include "types.h"
#include "hashmap.h"
#include "std/memory.h"

static int chm_bytewise_eq(const void* a, uint64_t alen, const void* b, uint64_t blen){
    if (alen!= blen) return 0;
    const uint8_t* p= (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    for(uint64_t i = 0; i < alen; i++) if (p[i] != q[i]) return 0;
    return 1;
}

uint64_t chashmap_fnv1a64(const void* data, uint64_t len){
    const uint8_t* bytes = (const uint8_t*)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint64_t i = 0; i < len; i++) {
        h^= (uint64_t)bytes[i];
        h*= 0x100000001b3ULL; 
    }
    return h;
}

static void* chm_alloc(const chashmap_t* map, uint64_t sz){
    if (map && map->alloc) return map->alloc(sz);
    return malloc(sz);
}

static void chm_free(const chashmap_t* map, void* ptr, uint64_t sz){
    if (map && map->free){
        map->free(ptr, sz);
        return;
    }
    free_sized(ptr,sz);
}

static uint64_t chm_next_pow2(uint64_t x){
    if (x<8) return 8;
    x--;
    x|=x>>1;
    x|=x>>2;
    x|=x>>4;
    x|=x>>8;
    x|=x>>16;
    x|=x>>32;
    return x + 1;
}

static void chm_update_threshold(chashmap_t* map){
    map->resize_threshold = (map->capacity*3)/4;
}

chashmap_t* chashmap_create_alloc(uint64_t initial_capacity, void* (*alloc)(size_t size),void (*mfree)(void* ptr, size_t size)){
    uint64_t cap = chm_next_pow2(initial_capacity ? initial_capacity : 8);
    chashmap_t* m = (chashmap_t*)alloc((uint64_t)sizeof(chashmap_t));

    if (!m) return 0;

    m->alloc = alloc; m->free = mfree;
    m->hash_fn = chashmap_fnv1a64;
    m->keyeq_fn = chm_bytewise_eq;
    m->value_dispose = 0;
    m->capacity = cap;
    m->size = 0;
    m->buckets = (chashmap_entry_t**)alloc((uint64_t)sizeof(chashmap_entry_t*)*cap);

    if (!m->buckets) {
        m->free(m, (uint64_t)sizeof(chashmap_t));
        return 0;
    }

    memset(m->buckets, 0, (uint64_t)sizeof(chashmap_entry_t*)*cap);
    chm_update_threshold(m);
    return m;
}

chashmap_t* chashmap_create(uint64_t initial_capacity){
    return chashmap_create_alloc(initial_capacity, malloc, free_sized);
}

void chashmap_destroy(chashmap_t* map){
    if(!map) return;

    for (uint64_t i = 0; i < map->capacity; i++) {
        chashmap_entry_t* e = map->buckets[i];
        while(e){
            chashmap_entry_t* n = e->next;
            if(e->key_len>0 && e->key) chm_free(map,e->key,e->key_len);
            if(map->value_dispose && e->value) map->value_dispose(e->value);
            chm_free(map, e, (uint64_t)sizeof(chashmap_entry_t));
            e = n;
        }
    }
    chm_free(map, map->buckets, (uint64_t)sizeof(chashmap_entry_t*)*map->capacity);
    chm_free(map, map, (uint64_t)sizeof(chashmap_t));
}

void chashmap_set_allocator(chashmap_t* map, void* (*alloc)(size_t), void (*dealloc)(void*, size_t)){
    if (!map) return;
    map->alloc = alloc;
    map->free = dealloc;
}

void chashmap_set_hash(chashmap_t* map, chashmap_hash_fn hash_fn, chashmap_keyeq_fn keyeq_fn){
    if (!map) return;
    if (hash_fn) map->hash_fn = hash_fn;
    if (keyeq_fn) map->keyeq_fn = keyeq_fn;
}

void chashmap_set_value_dispose(chashmap_t* map, void (*dispose_fn)(void*)){
    if(!map) return;
    map->value_dispose = dispose_fn;
}

static int chm_resize(chashmap_t* map, uint64_t new_capacity){
    uint64_t ncap = chm_next_pow2(new_capacity);
    chashmap_entry_t** nb = (chashmap_entry_t**)chm_alloc(map, (uint64_t)sizeof(chashmap_entry_t*)*ncap);
    if (!nb) return 0;

    memset(nb, 0, (uint64_t)sizeof(chashmap_entry_t*)*ncap);
    for (uint64_t i = 0; i < map->capacity; i++) {
        chashmap_entry_t* e = map->buckets[i];
        while (e) {
            chashmap_entry_t* nxt = e->next;
            uint64_t idx = e->hash & (ncap-1);
            e->next = nb[idx];
            nb[idx] = e;
            e = nxt;
        }
    }
    chm_free(map, map->buckets, (uint64_t)sizeof(chashmap_entry_t*)*map->capacity);
    map->buckets = nb;
    map->capacity = ncap;
    chm_update_threshold(map);

    return 1;
}

int chashmap_put(chashmap_t* map, const void* key, uint64_t key_len, void* value){
    if (!map) return -1;

    if (map->size >= map->resize_threshold){
        if (!chm_resize(map, map->capacity << 1)) return -1;
    }
    uint64_t h = map->hash_fn(key,key_len);
    uint64_t idx = h & (map->capacity-1);
    chashmap_entry_t* e = map->buckets[idx];

    while(e){
        if(e->hash == h && map->keyeq_fn(e->key, e->key_len, key, key_len)){
            e->value = value;
            return 0;
        }
        e = e->next;
    }
    chashmap_entry_t* ne = (chashmap_entry_t*)chm_alloc(map, (uint64_t)sizeof(chashmap_entry_t));
    if (!ne) return -1;
    ne->hash = h;
    
    if (key_len>0) {
        void* k = chm_alloc(map, key_len);
        if (!k) { chm_free(map, ne, (uint64_t)sizeof(chashmap_entry_t)); return -1; }
        memcpy(k, key, key_len);
        ne->key = k;
        ne->key_len = key_len;
    } else {
        ne->key = 0;
        ne->key_len = 0;
    }

    ne->value = value;
    ne->next = map->buckets[idx];
    map->buckets[idx] = ne;
    map->size++;

    return 1;
}

void* chashmap_get(const chashmap_t* map, const void* key, uint64_t key_len){
    if (!map) return 0;

    uint64_t h = map->hash_fn(key,key_len);
    uint64_t idx = h & (map->capacity-1);
    chashmap_entry_t* e = map->buckets[idx];

    while(e){
        if(e->hash == h && map->keyeq_fn(e->key, e->key_len, key, key_len)) return e->value;
        e = e->next;
    }
    return 0;
}

int chashmap_remove(chashmap_t* map, const void* key, uint64_t key_len, void** out_value){
    if (!map) return 0;

    uint64_t h = map->hash_fn(key, key_len);
    uint64_t idx = h & (map->capacity-1);
    chashmap_entry_t* e = map->buckets[idx];
    chashmap_entry_t* prev = 0;

    while(e){
        if (e->hash == h && map->keyeq_fn(e->key, e->key_len, key, key_len)) {
            if (prev) prev->next = e->next;
            else map->buckets[idx] = e->next;

            if (out_value) *out_value = e->value;
            else if(map->value_dispose && e->value) map->value_dispose(e->value);

            if (e->key_len>0 && e->key) chm_free(map, e->key, e->key_len);
            chm_free(map, e, (uint64_t)sizeof(chashmap_entry_t));
            map->size--;
            return 1;
        }
        prev = e;
        e = e->next;
    }

    return 0;
}

uint64_t chashmap_size(const chashmap_t* map){
    if(!map) return 0;
    return map->size;
}

uint64_t chashmap_capacity(const chashmap_t* map){
    if(!map) return 0;
    return map->capacity;
}

void chashmap_for_each(const chashmap_t* map, void (*func)(void* key, uint64_t key_len, void* value)){
    if (!map || !func) return;

    for (uint64_t i = 0; i < map->capacity; i++){
        chashmap_entry_t* e = map->buckets[i];
        while(e){
            chashmap_entry_t* next = e->next;
            func(e->key, e->key_len, e->value);
            e = next;
        }
    }
}

#include "alloc_tests.h"
#include "tests/test.h"
#include "memory/page_allocator.h"

bool test_kalloc_free(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint64_t), ALIGN_16B, MEM_PRIV_KERNEL);
    kfree(mem, sizeof(uint64_t));
    assert_eq(*(uint64_t*)mem, sizeof(uint64_t), "Memory not freed: %x",*(uint64_t*)mem);
    return true;
}

bool test_kalloc_alignment_free(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint8_t), ALIGN_16B, MEM_PRIV_KERNEL);
    kfree(mem, sizeof(uint8_t));
    assert_eq(*(uint64_t*)mem, ALIGN_16B, "Aligned memory not fully freed: %x",*(uint64_t*)mem);
    return true;
}

bool test_page_kalloc_free(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint64_t), ALIGN_4KB, MEM_PRIV_KERNEL);
    pfree(page, PAGE_SIZE);
    assert_eq(*(uint64_t*)mem, 0xDEADBEEFDEADBEEF, "Full page not freed: %x",mem);
    return true;
}

bool test_after_free(){
    uint64_t *a = malloc(64);
    a[3] = 12345678;
    free(a,64);
    assert_eq(a[3], 0xDEADBEEFDEADBEEF, "Use after free failed: %x",a[3]);
    return true;
}

bool alloc_tests(){
    return 
    test_after_free() &&
    test_kalloc_free() &&
    test_kalloc_alignment_free() &&
    test_page_kalloc_free() && 
    true;
}
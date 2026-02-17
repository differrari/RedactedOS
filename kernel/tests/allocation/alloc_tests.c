#include "alloc_tests.h"
#include "debug/assert.h"
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
    //potential fix: after aligning, put a free block on the upper part. Still, not fully freeing the memory before it
    assert_eq(*(uint64_t*)mem, ALIGN_16B, "Aligned memory not fully freed: %x",*(uint64_t*)mem);
    return true;
}

bool test_page_kalloc_no_free_unmanaged(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint64_t), ALIGN_4KB, MEM_PRIV_KERNEL);
    pfree(page, PAGE_SIZE);
    assert_true(page_used((uintptr_t)mem), "Page should not have been freed: %x",(uint64_t)mem);
    return true;
}

bool test_page_kalloc_free_managed(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint64_t), ALIGN_4KB, MEM_PRIV_KERNEL);
    free_managed_page(page);
    assert_false(page_used((uintptr_t)mem), "Full page not freed: %x",(uint64_t)mem);
    return true;
}

bool test_after_free(){
    uint64_t *a = malloc(64);
    a[3] = 12345678;
    free_sized(a,64);
    assert_eq(a[3], 0xDEADBEEFDEADBEEF, "Use after free failed: %x",a[3]);
    return true;
}

bool alloc_tests(){
    return 
    test_after_free() &&
    test_kalloc_free() &&
    test_page_kalloc_free_managed() && 
    test_page_kalloc_no_free_unmanaged() && 
    test_kalloc_alignment_free() &&
    true;
}
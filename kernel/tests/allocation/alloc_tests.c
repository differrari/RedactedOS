#include "alloc_tests.h"
#include "debug/assert.h"
#include "memory/page_allocator.h"

bool test_kalloc_free(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint64_t), ALIGN_16B, MEM_PRIV_KERNEL);
    kfree(mem, sizeof(uint64_t));
    assert_eq(*(uint64_t*)mem, 0xDEADBEEFDEADBEEF,  "Freed memory not poisoned: %llx", (uint64_t*)mem);
    free_managed_page(page);
    return true;
}

bool test_kalloc_alignment_free(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint8_t), ALIGN_16B, MEM_PRIV_KERNEL);
    void *mem1 = kalloc(page, sizeof(uint8_t), ALIGN_16B, MEM_PRIV_KERNEL);
    void *mem2 = kalloc(page, sizeof(uint8_t), ALIGN_16B, MEM_PRIV_KERNEL);
    assert_eq((uintptr_t)mem & (ALIGN_16B - 1), 0, "a not aligned: %llx",(uint64_t)mem);
    assert_eq((uintptr_t)mem1 & (ALIGN_16B - 1), 0, "mem1 not aligned: %llx", (uint64_t)mem1);
    assert_eq((uintptr_t)mem2 & (ALIGN_16B - 1), 0, "mem2 not aligned: %llx", (uint64_t)mem2);
    assert_true((uintptr_t)mem1 > (uintptr_t)mem + 1, "no alignment pad before b");
    assert_true((uintptr_t)mem2 > (uintptr_t)mem1 + 1, "no alignment pad after b");

    kfree(mem1, sizeof(uint8_t));
    assert_eq(*(uint64_t*)mem1, 0xDEADBEEFDEADBEEFULL, "Aligned freed memory not poisoned: %llx", *(uint64_t*)mem1);
    void *d = kalloc(page, 16, ALIGN_16B, MEM_PRIV_KERNEL);
    assert_eq((uintptr_t)d, (uintptr_t)mem1, "Aligned span not reused: %llx", (uint64_t)d);
    free_managed_page(page);
    return true;
}

bool test_page_kalloc_no_free_unmanaged(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint64_t), ALIGN_4KB, MEM_PRIV_KERNEL);
    pfree(page, PAGE_SIZE);
    assert_true(page_used((uintptr_t)mem), "Page should not have been freed: %x",(uint64_t)mem);
    free_managed_page(page);
    return true;
}

bool test_page_kalloc_free_managed(){
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *mem = kalloc(page, sizeof(uint64_t), ALIGN_4KB, MEM_PRIV_KERNEL);
    free_managed_page(page);
    assert_false(page_used((uintptr_t)mem), "Full page not freed: %x",(uint64_t)mem);
    return true;
}

//NOTE these reuse tests assume a specific reuse policy
//if that policy changes, these tests may no longer succeed even if the reallocation works
bool test_palloc_reuse_single() {
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    assert_true(page != 0, "single page allocation failed");
    assert_true(page_used((uintptr_t)page), "single page not marked used");
    pfree(page, PAGE_SIZE);
    assert_false(page_used((uintptr_t)page), "single page not freed");
    void *page2 = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    assert_eq((uintptr_t)page2, (uintptr_t)page, "single page not reused: %llx", (uintptr_t)page2);
    pfree(page2, PAGE_SIZE);
    return true;
}

bool test_palloc_reuse_gap() {
    void *a = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *b = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *c = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    assert_true(a && b && c, "gap allocation failed");
    pfree(b, PAGE_SIZE);
    void *d = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    assert_eq((uintptr_t)d, (uintptr_t)b, "gap page not reused: %llx", (uint64_t)d); 
    pfree(a, PAGE_SIZE);
    pfree(c, PAGE_SIZE);
    pfree(d, PAGE_SIZE);
    return true;
}

bool test_palloc_large_reuse() {
    size_t amount = PAGE_SIZE * 80;
    uint64_t *mem = (uint64_t*)palloc(amount, MEM_PRIV_KERNEL, MEM_RW, false);
    for (size_t i = 0; i < amount / sizeof(uint64_t); i += PAGE_SIZE / sizeof(uint64_t)) mem[i] = 0xA5A5000000000000ULL | i;
    pfree(mem, amount);
    uint64_t *mem2 = (uint64_t*)palloc(amount, MEM_PRIV_KERNEL, MEM_RW, false);
    assert_eq((uintptr_t)mem2, (uintptr_t)mem, "allocation not reused: %llx", (uint64_t)mem2);
    pfree(mem2, amount);
    return true;
}

bool test_kalloc_fragment_reuse() {
    void *page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    void *a = kalloc(page, 64, ALIGN_16B, MEM_PRIV_KERNEL);
    void *b = kalloc(page, 128, ALIGN_16B, MEM_PRIV_KERNEL);
    void *c = kalloc(page, 64, ALIGN_16B, MEM_PRIV_KERNEL);
    kfree(b, 128);
    void *d = kalloc(page, 96, ALIGN_16B, MEM_PRIV_KERNEL);
    assert_true((uintptr_t)d >= (uintptr_t)b && (uintptr_t)d + 96 <= (uintptr_t)b + 128, "allocation not placed inside freed fragment: %llx", (uint64_t)d);

    free_managed_page(page);
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
    test_palloc_reuse_single() &&
    test_palloc_reuse_gap() &&
    test_palloc_large_reuse() &&
    test_kalloc_fragment_reuse() &&
    test_kalloc_free() &&
    test_page_kalloc_free_managed() && 
    test_page_kalloc_no_free_unmanaged() && 
    test_kalloc_alignment_free() &&
    true;
}
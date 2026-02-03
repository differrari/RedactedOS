#include "syscalls/syscalls.h"
#include "test.h"
#include "alloc/allocate.h"

bool test_zalloc(){
    
    uintptr_t first = (uintptr_t)zalloc(0xCF8);//Heap + 0
    assert_eq(first & 0xFF, 0x30, "First allocation in wrong place %llx",first);
    print("First allocation at %llx",first);
    uintptr_t three_pages = (uintptr_t)zalloc(0x29f8);//Heap + 1 to Heap + 3
    assert_false(three_pages & 0xFFF, "Multi-page allocation not aligned correctly %x",three_pages & 0xFFF);
    assert_eq((three_pages & ~(0xFFF)), (first & ~(0xFFF)) + 0x1000, "Multi-page allocation not allocated in correct place %llx",three_pages);
    print("Multi-page allocation at %llx - %llx",(three_pages & ~(0xFFF)),(three_pages & ~(0xFFF)) + 0x3000);
    uintptr_t overflow = (uintptr_t)zalloc(0x3f8);//Heap + 4
    assert_eq(overflow & 0xFF, 0x30, "Overflown allocation not placed in new page %llx",overflow);
    assert_eq((overflow & ~(0xFFF)), (first & ~(0xFFF)) + 0x4000, "Multi-page allocation not aligned correctly");
    print("Second in-page alloc page at %llx",(overflow & ~(0xFFF)));
    uintptr_t in_page = (uintptr_t)zalloc(0x3f8);
    assert_eq((overflow & ~(0xFFF)), (in_page & ~(0xFFF)), "Allocation meant for %llx found in %llx",(overflow & ~(0xFFF)),(in_page & ~(0xFFF)));
    
    for (size_t i = first; i < 0x5000; i++){
        assert_false(*(uint8_t*)(first + i), "Allocated memory not 0'd %x",*(uint8_t*)(first + i));
    }
    
    print("Malloc with virtual memory working correctly");
    
    return true;
}

bool test_wrong_file_descriptor(){
    file descriptor = {};
    FS_RESULT res = openf("/console", &descriptor);
    assert_true(res, "Failed to open console file");
    descriptor.id = -2;
    void* file_img = malloc(descriptor.size);
    
    assert_eq(readf(&descriptor, file_img, descriptor.size), 0, "Read from non-owned fd: %i", descriptor.id);
    assert_eq(writef(&descriptor,file_img, descriptor.size), 0, "Wrote to non-owned fd: %i", descriptor.id);
    
    for (int i = 0; i < 100; i++)
        assert_false(((uint32_t*)file_img)[i],"Read non-zero");
    
    print("Successfully failed to read from non-owned file");
    return true;
}

int main(int argc, char *argv[]){
    return test_malloc() && test_wrong_file_descriptor() ? 0 : 1;
}
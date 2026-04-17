#include "syscalls/syscalls.h"
#include "debug/assert.h"
#include "alloc/allocate.h"

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
    return test_zalloc() && test_wrong_file_descriptor() ? 0 : 1;
}
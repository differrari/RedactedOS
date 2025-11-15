#include "alloc_tests.h"
#include "tests/test.h"

bool test_after_free(){
    uint64_t *a = malloc(64);
    a[3] = 12345678;
    free(a,64);
    assert_eq(a[3], 0xDEADBEEFDEADBEEF, "Use after free failed: %x",a[3]);
    return true;
}

bool alloc_tests(){
    return test_after_free();
}
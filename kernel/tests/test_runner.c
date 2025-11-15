#include "test_runner.h"
#include "allocation/alloc_tests.h"

bool run_tests(){
    return alloc_tests();
}
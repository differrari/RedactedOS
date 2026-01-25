#include "test_runner.h"
#include "allocation/alloc_tests.h"
#include "console/kio.h"

extern bool run_redlib_tests();

bool run_tests(){
    return alloc_tests() &&
    run_redlib_tests() &&
    true;
}
#pragma once

#include "types.h"
#include "syscalls/syscalls.h"

static inline bool assert_true_func(bool condition, char *fmt, ...){
    if (condition) return true;
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    char li[256]; 
    size_t n = string_format_va_buf(fmt, li, sizeof(li), args);
    va_end(args);
    if (n >= sizeof(li)) li[sizeof(li)-1] = '\0';
    printl(li);
    return false;
}

#define assert_true(cond, fmt, ...) if (!assert_true_func(cond, fmt, ##__VA_ARGS__)) return false;
#define assert_false(cond, fmt, ...) if (!assert_true_func(!cond, fmt, ##__VA_ARGS__)) return false;
#define assert_eq(el1, el2, fmt, ...) if (!assert_true_func(el1 == el2, fmt, ##__VA_ARGS__)) return false;
#define assert_neq(el1, el2, fmt, ...) if (!assert_true_func(el1 != el2, fmt, ##__VA_ARGS__)) return false;
#define test_fail(fmt, ...) if (!assert_true_func(false, fmt, ##__VA_ARGS__)) return false;
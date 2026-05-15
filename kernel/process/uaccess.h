#pragma once

#include "types.h"
#include "process/process.h"

#define UACCESS_MAX_ARGV 128

typedef enum uaccess_result {
    UACCESS_OK = 0,
    UACCESS_EPERM = -1,
    UACCESS_ENOMEM = -2,
    UACCESS_EFAULT = -3,
    UACCESS_EINVAL = -4,
    UACCESS_ENAMETOOLONG = -5,
} uaccess_result_t;

typedef struct user_argv {
    int argc;
    const char *argv[UACCESS_MAX_ARGV + 1];
    char *bufs[UACCESS_MAX_ARGV];
    uint64_t bufsz[UACCESS_MAX_ARGV];
} user_argv_t;

bool access_ok_range(process_t *proc, uintptr_t addr, size_t size, bool want_write);
bool validate_address(process_t *proc, uintptr_t addr, size_t size, bool want_write);
uaccess_result_t copy_from_user(process_t *proc, void *dst, uintptr_t src, size_t size);
uaccess_result_t copy_to_user(process_t *proc, uintptr_t dst, const void *src, size_t size);
uaccess_result_t copy_str_from_user(process_t *proc, char *dst, size_t dst_size, uintptr_t src, size_t *out_copied, bool *out_terminated);
uaccess_result_t copy_argv_from_user(process_t *proc, int argc, uintptr_t uargv, user_argv_t *out);
void free_argv_from_user(user_argv_t *argv);

#pragma once

#include "types.h"
#include "syscalls/errno.h"
#include "process/process.h"

typedef enum uaccess_result {
    UACCESS_OK = 0,
    UACCESS_EPERM = -SYSCALL_EPERM,
    UACCESS_ENOMEM = -SYSCALL_ENOMEM,
    UACCESS_EFAULT = -SYSCALL_EFAULT,
    UACCESS_EINVAL = -SYSCALL_EINVAL,
    UACCESS_ENAMETOOLONG = -SYSCALL_ENAMETOOLONG,
} uaccess_result_t;

bool access_ok_range(process_t *proc, uintptr_t addr, size_t size, bool want_write);
uaccess_result_t copy_from_user(process_t *proc, void *dst, uintptr_t src, size_t size);
uaccess_result_t copy_to_user(process_t *proc, uintptr_t dst, const void *src, size_t size);
uaccess_result_t copy_str_from_user(process_t *proc, char *dst, size_t dst_size, uintptr_t src, size_t *out_copied, bool *out_terminated);

#include "process/uaccess.h"
#include "memory/mm_process.h"
#include "memory/mmu.h"
#include "memory/addr.h"
#include "process/process.h"
#include "std/memory.h"
#include "syscalls/errno.h"

bool access_ok_range(process_t *proc, uintptr_t addr, size_t size, bool want_write) {
    if (!proc) return false;
    if (!proc->mm.ttbr0) return false;
    if (!size) return true;

    if ((addr >> 47) & 1) return false;

    uintptr_t end = addr + size - 1;
    if (end < addr) return false;
    if ((end >> 47) & 1) return false;

    uintptr_t cur = addr;
    while (cur <= end) {
        vma *m = mm_find_vma(&proc->mm, cur);
        if (!m) return false;
        if (want_write && !(m->prot & MEM_RW)) return false;

        uintptr_t next = m->end;
        if (!next || next - 1 >= end) return true;
        cur = next;
    }

    return true;
}

uaccess_result_t copy_from_user(process_t *proc, void *dst, uintptr_t src, size_t size) {
    if (!dst && size) return UACCESS_EINVAL;
    if (!size) return UACCESS_OK;
    if (!access_ok_range(proc, src, size, false)) return UACCESS_EFAULT;

    uint8_t *d = (uint8_t*)dst;

    while (size) {
        size_t off = src & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - off;
        if (chunk > size) chunk = size;

        int st = 0;
        uintptr_t pa =mmu_translate(src, &st);
        if (st) {
            uint64_t esr = (0x24ULL << 26) | 0x7ULL;
            if (!mm_try_handle_page_fault(proc, src, esr)) return UACCESS_EFAULT;

            pa = mmu_translate(src, &st);
            if (st) return UACCESS_EFAULT;
        }

        memcpy(d, (const void*)dmap_pa_to_kva((paddr_t)pa), chunk);
        d += chunk;
        src += chunk;
        size -= chunk;
    }

    return UACCESS_OK;
}

uaccess_result_t copy_to_user(process_t *proc, uintptr_t dst, const void *src, size_t size) {
    if (!src && size) return UACCESS_EINVAL;
    if (!size) return UACCESS_OK;
    if (!access_ok_range(proc, dst, size, true)) return UACCESS_EFAULT;

    const uint8_t *s = (const uint8_t*)src;

    while (size) {
        size_t off = dst & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - off;
        if (chunk > size) chunk = size;

        int st = 0;
        uintptr_t pa = mmu_translate(dst, &st);
        if (st) {
            uint64_t esr = (0x24ULL << 26) | 0x7ULL;
            esr |= (1 << 6);
            if (!mm_try_handle_page_fault(proc, dst, esr)) return UACCESS_EFAULT;

            pa = mmu_translate(dst, &st);
            if (st) return UACCESS_EFAULT;
        }

        memcpy((void*)dmap_pa_to_kva((paddr_t)pa), s, chunk);
        s += chunk;
        dst += chunk;
        size -= chunk;
    }

    return UACCESS_OK;
}

uaccess_result_t copy_str_from_user(process_t *proc, char *dst, size_t dst_size, uintptr_t src, size_t *out_copied, bool *out_terminated) {
    if (!dst || dst_size < 2) return UACCESS_EINVAL;

    size_t pos = 0;
    bool term = false;

    while (pos + 1 < dst_size) {
        uint8_t c = 0;
        uaccess_result_t r = copy_from_user(proc, &c, src + pos, 1);
        if (r != UACCESS_OK) return r;
        dst[pos] = (char)c;
        pos++;
        if(!c) {
            term = true;
            break;
        }
    }

    if (!term) dst[dst_size-1] = 0; 

    if (out_copied) *out_copied = term ? pos : (dst_size - 1);
    if (out_terminated) *out_terminated = term;
    return term ? UACCESS_OK : UACCESS_ENAMETOOLONG;
}
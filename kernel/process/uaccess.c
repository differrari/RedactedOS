#include "process/uaccess.h"
#include "memory/mm_process.h"
#include "memory/mmu.h"
#include "memory/addr.h"
#include "std/memory.h"
#include "alloc/allocate.h"

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
        uintptr_t pa =mmu_translate((uint64_t*)proc->mm.ttbr0, src, &st);
        if (st) {
            uint64_t esr = (0x24ULL << 26) | 0x7ULL;
            if (!mm_try_handle_page_fault(proc, src, esr)) return UACCESS_EFAULT;

            pa = mmu_translate((uint64_t*)proc->mm.ttbr0, src, &st);
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
        uintptr_t pa = mmu_translate((uint64_t*)proc->mm.ttbr0, dst, &st);
        if (st) {
            uint64_t esr = (0x24ULL << 26) | 0x7ULL | (1 << 6);
            if (!mm_try_handle_page_fault(proc, dst, esr)) return UACCESS_EFAULT;

            pa = mmu_translate((uint64_t*)proc->mm.ttbr0, dst, &st);
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
    if (out_copied) *out_copied = 0;
    if (out_terminated) *out_terminated = false;
    if (!dst || !dst_size) return UACCESS_EINVAL;
    if (!proc || !proc->mm.ttbr0) return UACCESS_EFAULT;
    if ((src >> 47) & 1) return UACCESS_EFAULT;

    size_t pos = 0;

    while (pos + 1 < dst_size) {
        size_t chunk = PAGE_SIZE - ((src + pos) & (PAGE_SIZE - 1));
        if (chunk > dst_size - 1 - pos) chunk = dst_size - 1 - pos;
        if (!access_ok_range(proc, src + pos, chunk, false)) return UACCESS_EFAULT;

        int st = 0;
        uintptr_t pa = mmu_translate((uint64_t*)proc->mm.ttbr0, src + pos, &st);
        if (st) {
            if (!mm_try_handle_page_fault(proc, src + pos, (0x24ULL << 26) | 0x7ULL)) return UACCESS_EFAULT;
            pa = mmu_translate((uint64_t*)proc->mm.ttbr0, src + pos, &st);
            if (st) return UACCESS_EFAULT;
        }
        memcpy(dst + pos, (const void*)dmap_pa_to_kva((paddr_t)pa), chunk);
        for (size_t i = 0; i < chunk; i++) {
            if (dst[pos + i]) continue;
            if (out_copied) *out_copied = pos + i +1;
            if (out_terminated) *out_terminated = true;
            return UACCESS_OK;
        }

        pos += chunk;
    }

    dst[dst_size-1] = 0;
    if (out_copied) *out_copied = dst_size - 1;
    return UACCESS_ENAMETOOLONG;
}

uaccess_result_t copy_argv_from_user(process_t *proc, int argc, uintptr_t uargv, user_argv_t *out) {
    if (!out) return UACCESS_EINVAL;
    if (argc < 0 || argc > UACCESS_MAX_ARGV) return UACCESS_EINVAL;

    memset(out, 0,sizeof(*out));
    out->argc = argc;

    for (int i = 0; i < argc; i++) {
        uintptr_t up = 0;
        uaccess_result_t ur = copy_from_user(proc, &up, uargv + ((uintptr_t)i * sizeof(uintptr_t)), sizeof(up));
        if (ur != UACCESS_OK) {
            free_argv_from_user(out);
            return ur;
        }
        if (!up) {
            free_argv_from_user(out);
            return UACCESS_EINVAL;
        }

        char tmp[256] = {};
        size_t copied = 0;
        bool term = false;
        ur = copy_str_from_user(proc, tmp, sizeof(tmp), up, &copied, &term);
        if (ur != UACCESS_OK) {
            free_argv_from_user(out);
            return ur;
        }
        if (!term) {
            free_argv_from_user(out);
            return UACCESS_ENAMETOOLONG;
        }

        char *k = (char*)zalloc(copied+1);
        if (!k) {
            free_argv_from_user(out);
            return UACCESS_ENOMEM;
        }

        memcpy(k, tmp, copied);
        k[copied] = 0;
        out->bufs[i] = k;
        out->bufsz[i] = copied+1;
        out->argv[i] = k;
    }
    return UACCESS_OK;
}

void free_argv_from_user(user_argv_t *argv) {
    if (!argv) return;
    for (int i = 0; i < argv->argc && i < UACCESS_MAX_ARGV; i++) {
        if (!argv->bufs[i]) continue;
        release(argv->bufs[i]);
        argv->bufs[i] = 0;
        argv->bufsz[i] = 0;
        argv->argv[i] = 0;
    }
    argv->argc = 0;
}
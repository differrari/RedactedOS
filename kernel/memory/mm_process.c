#include "process/process.h"
#include "memory/mmu.h"
#include "memory/addr.h"
#include "std/memory.h"
#include "memory/mm_process.h"

vma* mm_find_vma(mm_struct *mm, uaddr_t va){
    if (!mm) return 0;
    for (uint16_t i = 0; i < mm->vma_count; i++){
        vma *m = &mm->vmas[i];
        if (va < m->start) return 0;
        if (va < m->end) return m;
    }
    return 0;
}

bool mm_add_vma(mm_struct *mm, uaddr_t start, uaddr_t end, uint8_t prot, uint8_t kind, uint8_t flags){
    if (!mm) return false;
    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= end) return false;
    if (mm->vma_count >= MAX_VMAS) return false;

    uint16_t ins = 0;
    while (ins < mm->vma_count && mm->vmas[ins].start < start) ins++;

    if (ins > 0 && start < mm->vmas[ins - 1].end) return false;
    if (ins < mm->vma_count && end > mm->vmas[ins].start) return false;

    for (uint16_t i = mm->vma_count; i > ins; i--) mm->vmas[i] = mm->vmas[i - 1];

    mm->vmas[ins] = (vma){start, end, prot, kind, flags};
    mm->vma_count++;

    if (ins > 0) {
        vma *a = &mm->vmas[ins - 1];
        vma *b = &mm->vmas[ins];
        if (a->end == b->start && a->prot == b->prot && a->kind == b->kind && a->flags == b->flags && !(a->flags & VMA_FLAG_USERALLOC)) {
            a->end = b->end;
            for (uint16_t i = ins; i + 1 < mm->vma_count; i++) mm->vmas[i] = mm->vmas[i + 1];
            mm->vma_count--;
            ins--;
        }
    }

    if (ins + 1 < mm->vma_count) {
        vma *a = &mm->vmas[ins];
        vma *b = &mm->vmas[ins + 1];
        if (a->end == b->start && a->prot == b->prot && a->kind == b->kind && a->flags == b->flags && !(a->flags & VMA_FLAG_USERALLOC)) {
            a->end = b->end;
            for (uint16_t i = ins + 1; i + 1 < mm->vma_count; i++)mm->vmas[i] = mm->vmas[i + 1];
            mm->vma_count--;
        }
    }
    return true;
}

bool mm_remove_vma(mm_struct *mm, uaddr_t start, uaddr_t end) {
    if (!mm) return false;
    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= end) return false;

    for (uint16_t i = 0; i < mm->vma_count; i++) {
        vma *m = &mm->vmas[i];
        if (end <= m->start) return false;
        if (start >= m->end) continue;

        bool track_free = m->kind != VMA_KIND_HEAP && m->kind != VMA_KIND_STACK;
        uaddr_t free_start = 0;
        uaddr_t free_end = 0;

        if (start <= m->start && end >= m->end) {
            free_start = m->start;
            free_end = m->end;
            for (uint16_t j = i; j + 1 < mm->vma_count; j++) mm->vmas[j] = mm->vmas[j + 1];
            mm->vma_count--;
        } else if (start <= m->start) {
            free_start = m->start;
            free_end = end;
            m->start = end;
        } else if (end >= m->end) {
            free_start = start;
            free_end = m->end;
            m->end = start;
        } else {
            if (mm->vma_count >= MAX_VMAS) return false;
            free_start = start;
            free_end = end;
            for (uint16_t j = mm->vma_count; j > i + 1; j--) mm->vmas[j] = mm->vmas[j-1];
            mm->vmas[i + 1] = (vma){end, m->end, m->prot, m->kind, m->flags};
            m->end = start;
            mm->vma_count++;
        }

        if (!track_free) return true;

        free_start &= ~(PAGE_SIZE - 1);
        free_end = (free_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (free_start >= free_end) return true;

        uint16_t ins = 0;
        while (ins < mm->mmap_free_count && mm->mmap_free[ins].start < free_start) ins++;

        if (ins > 0 && mm->mmap_free[ins - 1].end >= free_start) {
            if (mm->mmap_free[ins - 1].end < free_end) mm->mmap_free[ins - 1].end = free_end;
            ins--;
        } else {
            if (mm->mmap_free_count >= MAX_VMAS) return true;
            for (uint16_t j = mm->mmap_free_count; j > ins; j--) mm->mmap_free[j] = mm->mmap_free[j - 1];
            mm->mmap_free[ins] = (mm_free_range){free_start, free_end};
            mm->mmap_free_count++;
        }

        while (ins + 1 < mm->mmap_free_count && mm->mmap_free[ins].end >= mm->mmap_free[ins + 1].start) {
            if (mm->mmap_free[ins].end < mm->mmap_free[ins + 1].end) mm->mmap_free[ins].end = mm->mmap_free[ins + 1].end;
            for (uint16_t j = ins + 1; j + 1 < mm->mmap_free_count; j++) mm->mmap_free[j] = mm->mmap_free[j + 1];
            mm->mmap_free_count--;
        }

        while (mm->mmap_free_count) {
            mm_free_range *top = &mm->mmap_free[mm->mmap_free_count - 1];
            if (top->end != mm->mmap_cursor) break;
            mm->mmap_cursor = top->start;
            mm->mmap_free_count--;
        }

        return true;
    }
    return false;
}

uaddr_t mm_alloc_mmap(mm_struct *mm, size_t size, uint8_t prot, uint8_t kind, uint8_t flags) {
    if (!mm) return 0;
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint16_t i = 0; i < mm->mmap_free_count; i++) {
        mm_free_range *r = &mm->mmap_free[i];
        size_t span = r->end - r->start;
        if(span < size) continue;
        uaddr_t base = (r->end - size) & ~(PAGE_SIZE - 1);
        if (base < r->start) continue;
        if (!mm_add_vma(mm, base, base + size, prot, kind, flags)) return 0;
        if (base == r->start && base + size == r->end) {
            for (uint16_t j = i; j + 1 < mm->mmap_free_count; j++) mm->mmap_free[j] = mm->mmap_free[j + 1];
            mm->mmap_free_count--;
        } else if (base == r->start) r->start += size;
        else r->end = base;
        return base;
    }

    uaddr_t heap_guard = mm->brk + (MM_GAP_PAGES * PAGE_SIZE);
    if (!mm->mmap_cursor) return 0;
    uaddr_t base = (mm->mmap_cursor - size) & ~(PAGE_SIZE - 1);
    if (base < heap_guard) return 0;
    if (base + size > mm->mmap_top) return 0;
    if (!mm_add_vma(mm, base, base + size, prot, kind, flags)) return 0;
    mm->mmap_cursor = base;
    return base;
}

bool mm_try_handle_page_fault(process_t *proc, uintptr_t far, uint64_t esr) {
    if (!proc || !proc->mm.ttbr0) return false;

    uint64_t ec = (esr >> 26) & 0x3F;
    uint32_t iss = esr & 0xFFFFFF;
    uint8_t ifsc = esr & 0x3F;

    bool is_exec = ec == 0x20;
    bool is_write = false;
    if (!is_exec) is_write = ((iss >> 6) & 1) != 0;

    if (ifsc >= 0x9 && ifsc <= 0xB) {
        if (!mmu_set_access_flag((uint64_t*)proc->mm.ttbr0, far)) return false;
        mmu_flush_asid(proc->mm.asid);
        return true;
    }

    if (ifsc >= 0xD && ifsc <= 0xF) return false;
    if (ifsc < 0x4 || ifsc > 0x7) return false;

    uintptr_t va_page = far & ~(PAGE_SIZE-1);
    vma *m = mm_find_vma(&proc->mm, va_page);
    if (!m) return false;
    if ((is_exec && !(m->prot & MEM_EXEC)) || (is_write && !(m->prot & MEM_RW))) return false;


    if (!(m->flags & VMA_FLAG_DEMAND)) return false;

    if (m->kind == VMA_KIND_STACK) {
        uintptr_t sp = proc->sp & ~(PAGE_SIZE - 1);
        uintptr_t low = sp > (32 * PAGE_SIZE) ? sp - (32 * PAGE_SIZE) : proc->mm.stack_limit;
        if (va_page < low || va_page < proc->mm.stack_limit || va_page >= proc->mm.stack_top) return false;
        uintptr_t grow_to = proc->mm.stack_commit;
        if (va_page < grow_to) grow_to = va_page;
        if (((proc->mm.stack_top - grow_to) / PAGE_SIZE) > proc->mm.cap_stack_pages) return false;
        for (uintptr_t page = proc->mm.stack_commit - PAGE_SIZE; page >= grow_to; page -= PAGE_SIZE) {
            paddr_t phys = palloc_inner(PAGE_SIZE, MEM_PRIV_USER, MEM_RW, true, false);
            if (!phys) {
                for (uintptr_t undo = page + PAGE_SIZE; undo < proc->mm.stack_commit; undo += PAGE_SIZE) {
                    uint64_t pa = 0;
                    if (!mmu_unmap_and_get_pa((uint64_t*)proc->mm.ttbr0, undo, &pa)) continue;
                    pfree((void*)dmap_pa_to_kva((paddr_t)pa), PAGE_SIZE);
                    if (proc->mm.rss_stack_pages) proc->mm.rss_stack_pages--;
                }
                return false;
            }
            memset((void*)dmap_pa_to_kva(phys), 0, PAGE_SIZE);
            mmu_map_4kb((uint64_t*)proc->mm.ttbr0, page, phys, MAIR_IDX_NORMAL, m->prot | MEM_NORM, MEM_PRIV_USER);
            proc->mm.rss_stack_pages++;
            if (page == 0) break;
        }
        proc->mm.stack_commit = grow_to;
        mmu_flush_asid(proc->mm.asid);
        return true;
    }

    if ((m->kind == VMA_KIND_HEAP && proc->mm.rss_heap_pages >= proc->mm.cap_heap_pages) || (m->kind == VMA_KIND_ANON && proc->mm.rss_anon_pages >= proc->mm.cap_anon_pages)) return false;

    paddr_t phys = palloc_inner(PAGE_SIZE, MEM_PRIV_USER, MEM_RW, true, false);
    if (!phys) return false;

    if (m->kind != VMA_KIND_ANON || (m->flags & VMA_FLAG_ZERO)) memset((void*)dmap_pa_to_kva(phys), 0, PAGE_SIZE);
    mmu_map_4kb((uint64_t*)proc->mm.ttbr0, va_page, phys, MAIR_IDX_NORMAL, m->prot | MEM_NORM, MEM_PRIV_USER);
    mmu_flush_asid(proc->mm.asid);

    if (m->kind == VMA_KIND_HEAP) proc->mm.rss_heap_pages++;
    else if (m->kind == VMA_KIND_ANON) proc->mm.rss_anon_pages++;

    return true;
}

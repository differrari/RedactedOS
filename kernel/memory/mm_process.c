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
    start = start & ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= end) return false;
    if (mm->vma_count >= MAX_VMAS) return false;

    uint16_t ins = 0;
    while (ins < mm->vma_count && mm->vmas[ins].start < start) ins++;

    if (ins > 0) {
        vma *p = &mm->vmas[ins - 1];
        if (start< p->end) return false;
    }
    if (ins < mm->vma_count) {
        vma *n = &mm->vmas[ins];
        if (end > n->start) return false;
    }

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

bool mm_update_vma(mm_struct *mm, uaddr_t start, uaddr_t end) {
    if (!mm) return false;
    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= end) return false;

    for (uint16_t i = 0; i < mm->vma_count; i++) {
        vma *m = &mm->vmas[i];
        if (m->start != start) continue;

        if (i > 0) {
            vma *p = &mm->vmas[i - 1];
            if (start < p->end) return false;
        }
        if (i + 1 < mm->vma_count) {
            vma *n = &mm->vmas[i + 1];
            if (end > n->start) return false;
        }

        m->end = end;
        if (i > 0) {
            vma *a = &mm->vmas[i - 1];
            vma *b = &mm->vmas[i];
            if (a->end == b->start && a->prot == b->prot && a->kind == b->kind && a->flags == b->flags && !(a->flags & VMA_FLAG_USERALLOC)) {
                a->end = b->end;
                for (uint16_t j = i; j + 1 < mm->vma_count; j++) mm->vmas[j] = mm->vmas[j + 1];
                mm->vma_count--;
                i--;
                m = &mm->vmas[i];
            }
        }

        if (i + 1 < mm->vma_count) {
            vma *a = &mm->vmas[i];
            vma *b = &mm->vmas[i + 1];
            if (a->end == b->start && a->prot == b->prot && a->kind == b->kind && a->flags == b->flags && !(a->flags & VMA_FLAG_USERALLOC)) {
                a->end = b->end;
                for (uint16_t j = i + 1; j + 1 < mm->vma_count; j++) mm->vmas[j] = mm->vmas[j + 1];
                mm->vma_count--;
            }
        }
        return true;
    }
    return false;
}

bool mm_remove_vma(mm_struct *mm, uaddr_t start, uaddr_t end) {
    if (!mm) return false;
    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= end) return false;

    for (uint16_t i = 0; i < mm->vma_count; i++) {
        vma *m = &mm->vmas[i];
        if (m->start != start) continue;
        if (m->end != end) continue;
        for (uint16_t j = i; j + 1 < mm->vma_count; j++) mm->vmas[j] = mm->vmas[j + 1];
        mm->vma_count--;
        return true;
    }
    return false;
}

uaddr_t mm_alloc_mmap(mm_struct *mm, size_t size, uint8_t prot, uint8_t kind, uint8_t flags) {
    if (!mm) return 0;
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (!mm->mmap_cursor) return 0;
    uaddr_t base = (mm->mmap_cursor- size) & ~(PAGE_SIZE - 1);
    uaddr_t heap_guard = mm->brk + (MM_GAP_PAGES * PAGE_SIZE);
    if (base < heap_guard) return 0;
    if (base + size > mm->mmap_top) return 0;
    if (!mm_add_vma(mm, base, base + size, prot, kind, flags)) return 0;
    mm->mmap_cursor = base;
    return base;
}

bool mm_try_handle_page_fault(process_t *proc, uintptr_t far, uint64_t esr) {
    if (!proc) return false;

    uint64_t ec = (esr >> 26) & 0x3F;
    uint32_t iss = esr & 0xFFFFFF;
    uint8_t ifsc = esr & 0x3F;

    bool is_exec = ec == 0x20;
    bool is_write = false;
    if (!is_exec) is_write = ((iss >> 6) & 1) != 0;

    if (ifsc >= 0x9 && ifsc <= 0xB) {
        if (!mmu_set_access_flag((uint64_t*)proc->ttbr, far)) return false;
        mmu_flush_asid(proc->asid);
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
        if (va_page < proc->mm.stack_limit) return false;
        if (va_page >= proc->mm.stack_top) return false;

        if (va_page + PAGE_SIZE == proc->mm.stack_commit) {
            if (proc->mm.stack_commit <= proc->mm.stack_limit) return false;
            if (proc->mm.rss_stack_pages >= proc->mm.cap_stack_pages) return false;
            proc->mm.stack_commit -= PAGE_SIZE;
        } else if (va_page < proc->mm.stack_commit) return false;
        else if (proc->mm.rss_stack_pages >= proc->mm.cap_stack_pages) return false;
    } if ((m->kind == VMA_KIND_HEAP && proc->mm.rss_heap_pages >= proc->mm.cap_heap_pages) || (m->kind == VMA_KIND_ANON && proc->mm.rss_anon_pages >= proc->mm.cap_anon_pages)) return false;

    paddr_t phys = palloc_inner(PAGE_SIZE, MEM_PRIV_USER, MEM_RW, true, false);
    if (!phys) return false;

    if (m->kind != VMA_KIND_ANON || (m->flags & VMA_FLAG_ZERO)) memset((void*)dmap_pa_to_kva(phys), 0, PAGE_SIZE);
    mmu_map_4kb((uint64_t*)proc->ttbr, va_page, phys, MAIR_IDX_NORMAL, m->prot | MEM_NORM, MEM_PRIV_USER);
    mmu_flush_asid(proc->asid);

    if (m->kind == VMA_KIND_STACK) proc->mm.rss_stack_pages++;
    else if (m->kind == VMA_KIND_HEAP) proc->mm.rss_heap_pages++;
    else if (m->kind == VMA_KIND_ANON) proc->mm.rss_anon_pages++;

    return true;
}

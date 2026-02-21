#include "memory/mm_process.h"

static bool vma_can_merge(vma *a, vma *b) {
    if (!a || !b) return false;
    if (a->end != b->start) return false;
    if (a->prot != b->prot) return false;
    if (a->kind != b->kind) return false;
    if (a->flags != b->flags) return false;
    return true;
}

vma* mm_find_vma(mm_struct *mm, uintptr_t va){
    if (!mm) return 0;
    for (uint16_t i = 0; i < mm->vma_count; i++){
        vma *m = &mm->vmas[i];
        if (va < m->start) return 0;
        if (va < m->end) return m;
    }
    return 0;
}

bool mm_add_vma(mm_struct *mm, uintptr_t start, uintptr_t end, uint8_t prot, uint8_t kind, uint8_t flags){
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

    if (ins > 0 && vma_can_merge (&mm->vmas[ins - 1], &mm->vmas[ins])) {
        mm->vmas[ins - 1].end = mm->vmas[ins].end;
        for (uint16_t i = ins; i + 1 < mm->vma_count; i++) mm->vmas[i] = mm->vmas[i + 1];
        mm->vma_count--;
        ins--;
    }

    if (ins + 1 < mm->vma_count && vma_can_merge(&mm->vmas[ins], &mm->vmas[ins + 1])) {
        mm->vmas[ins].end = mm->vmas[ins + 1].end;
        for (uint16_t i = ins + 1; i + 1 < mm->vma_count; i++)mm->vmas[i] = mm->vmas[i + 1];
        mm->vma_count--;
    }
    return true;
}

bool mm_update_vma(mm_struct *mm, uintptr_t start, uintptr_t end) {
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
        if (i > 0 && vma_can_merge(&mm->vmas[i - 1], m)) {
            mm->vmas[i - 1].end = m->end;
            for (uint16_t j = i; j + 1 < mm->vma_count; j++) mm->vmas[j] = mm->vmas[j + 1];
            mm->vma_count--;
            i--;
            m = &mm->vmas[i];
        }

        if (i + 1 < mm->vma_count && vma_can_merge(m, &mm->vmas[i + 1])) {
            m->end = mm->vmas[i + 1].end;
            for (uint16_t j = i + 1; j + 1 < mm->vma_count; j++) mm->vmas[j] = mm->vmas[j + 1];
            mm->vma_count--;
        }
        return true;
    }
    return false;
}

uintptr_t mm_alloc_mmap(mm_struct *mm, size_t size, uint8_t prot, uint8_t kind, uint8_t flags) {
    if (!mm) return 0;
    if (!size) return 0;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (!mm->mmap_cursor) return 0;
    uintptr_t base = (mm->mmap_cursor- size) & ~(PAGE_SIZE - 1);
    if (base < mm->brk) return 0;
    if (base + size > mm->mmap_top) return 0;
    if (!mm_add_vma(mm, base, base + size, prot, kind, flags)) return 0;
    mm->mmap_cursor = base;
    return base;
}

#include "memory/mm_process.h"

vma* mm_find_vma(mm_struct *mm, uintptr_t va){
    if (!mm) return 0;
    for (uint16_t i = 0; i < mm->vma_count; i++){
        vma *m = &mm->vmas[i];
        if (va >= m->start && va < m->end) return m;
    }
    return 0;
}

bool mm_add_vma(mm_struct *mm, uintptr_t start, uintptr_t end, uint8_t prot, uint8_t kind, uint8_t flags){
    if (!mm) return false;
    start = start & ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (start >= end) return false;
    if (mm->vma_count >= MAX_VMAS) return false;
    mm->vmas[mm->vma_count++ ] = (vma){start, end, prot, kind, flags};
    return true;
}

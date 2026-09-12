#include "stack_manager.h"
#include "exceptions/exception_handler.h"
#include "memory/mmu.h"
#include <stddef.h>

uptr next_stack_addr(process_t *proc){
    if (is_privileged(proc)) return 0;
    for (uptr start = stack_max_addr; start > stack_min_addr; start -= stack_max + PAGE_SIZE){
        vma *m = mm_find_vma(&proc->mm, start-stack_max);
        if (!m){
            return start;
        }
    }
    print("Damn girl how many stacks u got");
    return 0;
}

void unmap_stack(process_t *proc, stack_t stack){
    vma *m = mm_find_vma(&proc->mm, stack.top-stack_max);
    if (m){
        for (uptr addr = stack.top-stack.size; addr < stack.top;  addr += PAGE_SIZE){
            uptr pa = 0;
            mmu_unmap_and_get_pa(proc->mm.ttbr0, addr, &pa);
            if (pa)
                pfree((void*)pa, PAGE_SIZE);
        }
        mm_remove_vma(&proc->mm, stack.top-stack.size, stack.top);
    }
}

stack_t new_stack(process_t *proc){
    uptr stack_top = 0;
    size_t size = 0;
    if (is_privileged(proc)) {
        size = kstack_max;
        void *st = palloc(size, MEM_PRIV_KERNEL, MEM_RW, true);
        stack_top = (uptr)st + size;
        register_allocation(proc->alloc_map, st, size);
    } else {
        stack_top = next_stack_addr(proc);
        size = stack_max;
    }
    if (!stack_top) return (stack_t){};
    uptr stack_limit = stack_top - size;
    print("New stack at %llx-%llx",stack_limit,stack_top);
    if (!is_privileged(proc) && !mm_add_vma(&proc->mm, stack_limit, stack_top, MEM_RW, VMA_KIND_STACK, VMA_FLAG_DEMAND))
        return (stack_t){};
    return (stack_t){.top = stack_top, .size = size, .max = size};
}
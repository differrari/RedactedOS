#include "stack_manager.h"
#include "exceptions/exception_handler.h"

uptr next_stack_addr(process_t *proc){
    if (is_privileged(proc)) return 0;
    for (uptr start = stack_max_addr; start > stack_min_addr; start -= 0x100000000000){
        vma *m = mm_find_vma(&proc->mm, start-stack_max);
        if (!m){
            return start;
        }
    }
    print("Damn girl how many stacks u got");
    return 0;
}

stack_t new_stack(process_t *proc){
    uptr stack_top = 0;
    if (is_privileged(proc)) {
        void *st = palloc(stack_max, MEM_PRIV_KERNEL, MEM_RW, true);
        stack_top = (uptr)st;
        register_allocation(proc->alloc_map, st, stack_max);
    } else stack_top = next_stack_addr(proc);
    if (!stack_top) return (stack_t){};
    uptr stack_limit = stack_top - stack_max;
    print("New stack at %llx-%llx",stack_limit,stack_top);
    if (!mm_add_vma(&proc->mm, stack_limit, stack_top, MEM_RW, VMA_KIND_STACK, VMA_FLAG_DEMAND))
        return (stack_t){};
    return (stack_t){.top = stack_top, .size = stack_max, .max = stack_max};
}
#include "kprocess_loader.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "interrupts/gic.h"

process_t *create_kernel_process(char *name, void (*func)()){

    disable_interrupt();
    
    process_t* proc = init_process();

    name_process(proc, name);

    uint64_t stack_size = 0x1000;

    uintptr_t stack = (uintptr_t)alloc_page(stack_size, true, false, false);
    kprintf_raw("Stack size %h. Start %h", stack_size,stack);
    if (!stack) return 0;

    uintptr_t heap = (uintptr_t)alloc_page(stack_size, true, false, false);
    kprintf_raw("Heap %h", heap);
    if (!heap) return 0;

    proc->stack = (stack + stack_size);
    proc->stack_size = stack_size;

    proc->heap = heap;

    proc->sp = proc->stack;
    
    proc->pc = (uintptr_t)func;
    kprintf_raw("Process allocated with address at %h, stack at %h",proc->pc, proc->sp);
    proc->spsr = 0x205;
    proc->state = READY;

    enable_interrupt();
    
    return proc;
}
#include "kprocess_loader.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "sysregs.h"

process_t *create_kernel_process(const char *name, int (*func)(int argc, char* argv[]), int argc, const char* argv[]){

    disable_interrupt();
    
    process_t* proc = init_process();

    name_process(proc, name);

    uint64_t stack_size = 0x1000;

    uintptr_t stack = (uintptr_t)palloc(stack_size, MEM_PRIV_KERNEL, MEM_RW, false);
    kprintf("Stack size %llx. Start %llx", stack_size,stack);
    if (!stack) return 0;

    uintptr_t heap = (uintptr_t)palloc(stack_size, MEM_PRIV_KERNEL, MEM_RW, false);
    kprintf("Heap %llx", heap);
    if (!heap) return 0;

    proc->stack = (stack + stack_size);
    proc->stack_size = stack_size;

    proc->heap = heap;

    proc->sp = proc->stack;
    
    proc->pc = PHYS_TO_VIRT(((uintptr_t)func));
    kprintf("Kernel process %s (%i) allocated with address at %llx, stack at %llx, heap at %llx. %i argument(s)", (uintptr_t)name, proc->id, proc->pc, proc->sp, proc->heap, argc);
    proc->spsr = 0x205;
    proc->state = READY;
    proc->PROC_X0 = argc;
    proc->PROC_X1 = (uintptr_t)argv;

    proc->output = PHYS_TO_VIRT((uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, true));

    enable_interrupt();
    
    return proc;
}
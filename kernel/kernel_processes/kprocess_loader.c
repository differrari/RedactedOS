#include "kprocess_loader.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "process/proc_allocator.h"
#include "interrupts/gic.h"

process_t *create_kernel_process(void (*func)()){

    disable_interrupt();
    
    process_t* proc = init_process();

    uint64_t stack_size = 0x1000;

    uint64_t stack = (uint64_t)alloc_proc_mem(stack_size, true);
    kprintf_raw("Stack size %h. Start %h", stack_size,stack);
    if (!stack) return 0;

    proc->stack = (stack + stack_size);
    proc->stack_size = stack_size;
    proc->sp = proc->stack;
    
    proc->pc = (uint64_t)func;
    kprintf_raw("Process allocated with address at %h, stack at %h",proc->pc, proc->sp);
    proc->spsr = 0x205;
    proc->state = READY;

    enable_interrupt();
    
    return proc;
}
#include "kprocess_loader.h"
#include "console/kio.h"
#include "scheduler.h"
#include "proc_allocator.h"

process_t *create_kernel_process(void (*func)(), uint64_t code_size){
    
    process_t* proc = init_process();

    uint64_t stack_size = 0x1000;

    uint64_t stack = (uint64_t)alloc_proc_mem(stack_size, true);
    kprintf_raw("Stack size %h. Start %h", stack_size,stack);
    if (!stack) return 0;

    proc->sp = (stack + stack_size);
    
    proc->pc = (uint64_t)func;
    kprintf_raw("Process allocated with address at %h, stack at %h",proc->pc, proc->sp);
    proc->spsr = 0x3C5;
    proc->state = READY;
    
    return proc;
}
#include "kprocess_loader.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "sysregs.h"
#include "string/string.h"
#include "memory/addr.h"
#include "memory/memory.h"

process_t *create_kernel_process(const char *name, int (*func)(int argc, char* argv[]), int argc, const char* argv[]){

    irq_flags_t irq = irq_save_disable();
    
    process_t* proc = init_process();
    if (!proc) {
        irq_restore(irq);
        return 0;
    }

    proc->alloc_map = make_page_index();
    if (!proc->alloc_map) {
        reset_process(proc);
        irq_restore(irq);
        return 0;
    }

    name_process(proc, name);

    uint64_t stack_size = 0x10000;

    uintptr_t stack = (uintptr_t)palloc(stack_size, MEM_PRIV_KERNEL, MEM_RW, true);
    if (!stack) {
        reset_process(proc);
        irq_restore(irq);
        return 0;
    }
    register_allocation(proc->alloc_map, (void*)stack, stack_size);

    uintptr_t heap = (uintptr_t)palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    if (!heap) {
        free_registered(proc->alloc_map, (void*)stack);
        reset_process(proc);
        irq_restore(irq);
        return 0;
    }
    register_allocation(proc->alloc_map, (void*)dmap_pa_to_kva(heap), PAGE_SIZE);

    proc->stack = (stack + stack_size);
    proc->stack_size = stack_size;

    proc->heap_phys = heap;

    proc->sp = proc->stack;
    
    proc->output = (kaddr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
    if (!proc->output) {
        reset_process(proc);
        irq_restore(irq);
        return 0;
    }
    proc->output_size = 0;
    
    proc->pc = PHYS_TO_VIRT((uintptr_t)func);
    proc->spsr = 0x205;

    proc->PROC_X0 = 0;
    proc->PROC_X1 = 0;

    if (argc > 0 && argv) {

        uint64_t argvs = (uint64_t)(argc + 1) * sizeof(char*);
        uint64_t str_total = 0;

        for (int i = 0; i < argc; i++) {
            if (!argv[i]) continue;
            str_total += (uint64_t)strlen(argv[i]) + 1;
        }

        uint64_t need = argvs + str_total;
        need = (need + 0xF) & ~0xFULL;

        if (need + 0x20 < stack_size) {

            uintptr_t top = proc->stack;
            uintptr_t base = (top - need) & ~0xFULL;

            char **kargv = (char**)base;
            char *kstr = (char*)(base + argvs);

            uint64_t off = 0;
            for (int i = 0; i < argc; i++) {

                if (!argv[i]) {
                    kargv[i] = 0;
                    continue;
                }

                uint64_t len = (uint64_t)strlen(argv[i]);
                memcpy(kstr + off, argv[i], len);
                kstr[off + len] = 0;

                kargv[i] = kstr + off;
                off += len + 1;
            }

            kargv[argc] = 0;

            proc->sp = base;
            proc->PROC_X0 = argc;
            proc->PROC_X1 = (uintptr_t)kargv;
        }
    }

    ready_process(proc);
    kprintf("Kernel process %s (%i) allocated with address at %llx, stack at %llx-%llx, heap at %llx. %i argument(s)", (uintptr_t)name, proc->id, proc->pc, proc->sp - proc->stack_size, proc->sp, (uaddr_t)dmap_pa_to_kva(proc->heap_phys), argc);
    irq_restore(irq);
    
    return proc;
}
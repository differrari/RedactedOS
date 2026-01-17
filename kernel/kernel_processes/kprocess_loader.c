#include "kprocess_loader.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "sysregs.h"
#include "std/std.h"

process_t *create_kernel_process(const char *name, int (*func)(int argc, char* argv[]), int argc, const char* argv[]){

    disable_interrupt();
    
    process_t* proc = init_process();

    name_process(proc, name);

    uint64_t stack_size = 0x10000;

    uintptr_t stack = (uintptr_t)palloc(stack_size, MEM_PRIV_KERNEL, MEM_RW, true);
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

    proc->output = PHYS_TO_VIRT((uintptr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true));

    enable_interrupt();
    
    return proc;
}
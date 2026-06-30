#include "kprocess_loader.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "sysregs.h"
#include "string/string.h"
#include "memory/addr.h"
#include "memory/memory.h"
#include "process/isolated_fs/isolated_fs.h"

void kernel_thread_return_trampoline(int32_t exit_code){
    switch_proc(YIELD);//TODO: proper cleanup
    while (true){}
}

void kernel_process_return_trampoline(int32_t exit_code) {
    stop_current_process(exit_code);
    while (true) {}
}

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

    uintptr_t heap = (uintptr_t)palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    if (!heap) {
        reset_process(proc);
        irq_restore(irq);
        return 0;
    }
    register_allocation(proc->alloc_map, (void*)dmap_pa_to_kva(heap), PAGE_SIZE);

    proc->heap_phys = heap;
    
    new_thread(proc, &proc->main_thread, 0x205, (uptr)func);

    if (argc > 0 && argv) {

        uint64_t argvs = (uint64_t)(argc + 1) * sizeof(char*);
        uint64_t str_total = 0;

        for (int i = 0; i < argc; i++) {
            if (!argv[i]) continue;
            str_total += (uint64_t)strlen(argv[i]) + 1;
        }

        uint64_t need = argvs + str_total;
        need = (need + 0xF) & ~0xFULL;

        if (need + 0x20 < proc->main_thread.stack_info.size) {

            uintptr_t top = proc->main_thread.stack_info.top;
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

            proc->main_thread.sp = base;
            proc->main_thread.PROC_X0 = argc;
            proc->main_thread.PROC_X1 = (uintptr_t)kargv;
        }
    }

    make_process_fs(proc, 0);

    ready_process(proc);
    kprintf("[NEW PROC:K] process %s (pid: %i main tid: %i) allocated with address at %llx, stack at %llx-%llx, heap at %llx. %i argument(s)", (uintptr_t)name, proc->id, proc->main_thread.tid, proc->main_thread.pc, proc->main_thread.sp - proc->main_thread.stack_info.size, proc->main_thread.sp, (uaddr_t)dmap_pa_to_kva(proc->heap_phys), argc);
    irq_restore(irq);
    
    return proc;
}
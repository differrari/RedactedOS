#include "debug.h"
#include "syscalls/syscalls.h"
#include "loading/elf_file.h"
#include "scheduler.h"
#include "console/kio.h"
#include "memory/mmu.h"
#include "memory/addr.h"
#include "loading/dwarf.h"

void debug_load(){
    kprint("[DEBUG] Loading debug information for kernel");
    file fd = {};
    if (openf("/boot/kernel.elf", &fd) != FS_RESULT_SUCCESS) {
        kprintf("[DEBUG] failed to open debug files");
        return;
    }

    kprint("[DEBUG] .elf fopened");

    void *file = zalloc(fd.size);

    kprintf("[DEBUG] alloc'd %x",fd.size);

    readf(&fd, file, fd.size);
    closef(&fd);

    kprintf("[DEBUG] Reading debug info from %x",file);

    get_elf_debug_info(get_kernel_proc(), file, fd.size);

    kprintf("[DEBUG] .debug_line %x .debug_line_str %x", get_kernel_proc()->debug_lines.ptr, get_kernel_proc()->debug_line_str.ptr);

}


bool decode_crash_address_with_info(uint8_t depth, uintptr_t address, sizedptr debug_line, sizedptr debug_line_str){
    if (!debug_line.ptr || !debug_line.size) return false;
    debug_line_info info = dwarf_decode_lines(debug_line.ptr, debug_line.size, debug_line_str.ptr, debug_line_str.size, address);
    if (info.address == address){
        kprintf("[%.16x] %i: %s %i:%i", address, depth, info.file, info.line, info.column);
        return true;
    }
    return false;
}

bool decode_crash_address(uint8_t depth, uintptr_t address, sizedptr debug_line, sizedptr debug_line_str){
    return decode_crash_address_with_info(depth, address, debug_line, debug_line_str) ||
    decode_crash_address_with_info(depth, address, get_kernel_proc()->debug_lines, get_kernel_proc()->debug_line_str);
}


void backtrace(uptr *ttbr, uintptr_t fp, uintptr_t elr, sizedptr debug_line, sizedptr debug_line_str) {

    if (elr){
        if (!decode_crash_address(0, elr, debug_line, debug_line_str))
            kprintf("Exception triggered by %llx",(elr));
    }

    for (uint8_t depth = 1; depth < 10 && fp; depth++) {
        int tr_ra = 0;
        uintptr_t ra_pa = mmu_translate(ttbr, fp + 8, &tr_ra);
        if (tr_ra) return;

        uintptr_t return_address = (*(uintptr_t*)dmap_pa_to_kva((paddr_t)ra_pa));
        if (!return_address) return;
        return_address -= 4;//Return address is the next instruction after branching
        if (!decode_crash_address(depth, return_address, debug_line, debug_line_str))
            kprintf("%i: caller address: %llx", depth, return_address);
        int tr = 0;
        uintptr_t fp_pa = mmu_translate(ttbr, fp, &tr);
        if (tr) return;
        fp = *(uintptr_t*)dmap_pa_to_kva((paddr_t)fp_pa);
    }
}

void debug_snapshot(process_t *proc, thread_t *t){
    backtrace(proc->mm.ttbr0, t->sp, t->pc, proc->debug_lines, proc->debug_line_str);
}

bool set_inspect(debug_inspect_types types, process_t *inspector, thread_t *inspector_thread, u16 inspected_pid, u16 inspected_tid){
    if (!inspector || !inspector_thread) return false;
    process_t *inspected = get_proc_by_pid(inspected_pid);
    if (!inspected || inspected->id != inspected_pid) return false;
    thread_t *inspected_thread = get_thread_from_proc(inspected, inspected_tid);
    if (!inspected_thread || inspected_thread->pid != inspected_pid || inspected_thread->tid != inspected_tid) return false;
    inspected_thread->inspector = (proc_addr){
        .pid = inspector->id,
        .tid = inspector_thread->tid
    };
    return true;
}
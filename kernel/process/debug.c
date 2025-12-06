#include "debug.h"
#include "syscalls/syscalls.h"
#include "loading/elf_file.h"
#include "scheduler.h"
#include "console/kio.h"

void debug_load(){
    kprint("[DEBUG] Loading debug information for kernel");
    file fd = {};
    if (openf("/boot/kernel.elf", &fd) != FS_RESULT_SUCCESS) {
        kprintf("[DEBUG] failed to open debug files");
        return;
    }

    kprint("[DEBUG] .elf fopened");

    void *file = malloc(fd.size);

    kprintf("[DEBUG] Malloced %x",fd.size);

    readf(&fd, file, fd.size);
    closef(&fd);

    kprintf("[DEBUG] Reading debug info from %x",file);

    get_elf_debug_info(get_proc_by_pid(0), file, fd.size);

    kprintf("[DEBUG] .debug_line %x .debug_line_str %x", get_proc_by_pid(0)->debug_lines.ptr, get_proc_by_pid(0)->debug_line_str.ptr);

}

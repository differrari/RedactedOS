#include "bin_mod.h"
#include "ping.h"
#include "shutdown.h"
#include "tracert.h"
#include "monitor_processes.h"
#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscalls.h"
#include "console/kio.h"
#include "process/loading/elf_file.h"
#include "memory/page_allocator.h"
#include "alloc/allocate.h"
#include "std/memory.h"
#include "sysregs.h"
#include "memory/addr.h"

bool init_bin(){
    return true;
}

typedef struct open_bin_ref {
    char *name;
    int (*func)(int argc, char* argv[]);
} open_bin_ref;

open_bin_ref available_cmds[] = {
    { "ping", run_ping },
    { "shutdown", run_shutdown },
    { "tracert", run_tracert },
    { "monitor", monitor_procs },
};

process_t* execute(const char* prog_name, int argc, const char* argv[]){
    if (!prog_name || !*prog_name) return 0;
    
    char *full_name = 0;
    full_name = (strend_case(prog_name, ".elf", true) == 0) ? string_from_literal(prog_name).data : strcat_new(prog_name, ".elf");
    if (full_name) {
        char pathbuf[1024] = {};
        size_t pathlen = string_format_buf(pathbuf, sizeof(pathbuf), "/boot/redos/bin/%s",full_name);
        process_t *proc = 0;
        if (pathlen < sizeof(pathbuf) - 1) proc = load_elf_process_path(prog_name, 0, pathbuf, argc, argv);
        release(full_name);
        if (proc) return proc;
    }

    for (uint32_t i = 0; i < N_ARR(available_cmds); i++){
        if (strcmp(available_cmds[i].name, prog_name) == 0){
            return create_kernel_process(available_cmds[i].name, available_cmds[i].func, argc, argv);
        }
    }
    return 0;
}

FS_RESULT open_bin(){
    return FS_RESULT_DRIVER_ERROR;
}

size_t read_bin(){
    return 0;
}

size_t list_bin(const char *path, void *buf, size_t size, file_offset offset){
    return 0;
}

system_module bin_module = (system_module){
    .name = "bin",
    .mount = "/bin",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_bin,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .sread = 0,
    .swrite = 0,
    .readdir = 0,
};//TODO: symlinks to link /bin to /boot/redos/bin

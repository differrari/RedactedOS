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
        char pathbuf[1024];
        size_t pathlen = string_format_buf(pathbuf, sizeof(pathbuf), "/boot/redos/bin/%s",full_name);
        if (pathlen >= sizeof(pathbuf) - 1) {
            release(full_name);
            return 0;
        }

        file fd = {};
        FS_RESULT op = openf(pathbuf, &fd);
        if (op == FS_RESULT_SUCCESS){
            char *program = zalloc(fd.size);
            if (!program){
                kprintf("Failed to read file %s", pathbuf);
                closef(&fd);
                release(full_name);
                return 0;
            }
            if (readf(&fd, program, fd.size) != fd.size){
                kprintf("Failed to read file %s", pathbuf);
                closef(&fd);
                release(program);
                release(full_name);
                return 0;
            }
            process_t *proc = load_elf_file(prog_name, 0, program, fd.size);
            closef(&fd);
            release(program);
            if (!proc){
                kprintf("Failed to create process for %s",prog_name);
                release(full_name);
                return 0;
            }
            proc->PROC_X0 = argc;
            proc->PROC_X1 = 0;
            if (argc > 0 && argv) {
                bool args_ok = true;
                size_t total_str = 0;
                for (int j = 0; j < argc; j++) {
                    const char *s = argv[j];
                    if (!s) {
                        args_ok = false;
                        break;
                    }
                    size_t l = strlen(s);
                    if (l > proc->stack_size) {
                        args_ok = false;
                        break;
                    }
                    if (total_str > proc->stack_size - (l + 1)) {
                        args_ok = false;
                        break;
                    }
                    total_str += l + 1;
                }

                size_t argvs = (size_t)argc * sizeof(uintptr_t);
                if (argvs > proc->stack_size) args_ok = false;
                if (total_str> proc->stack_size - argvs) args_ok = false;
                size_t total = total_str + argvs;
                if (total + 16 > proc->stack_size) args_ok = false;
                if (args_ok) {
                    char *nargvals = (char*)(PHYS_TO_VIRT_P(proc->stack_phys)-total);
                    char *vnargvals = (char*)(proc->stack-total);
                    char** nargv = (char**)(PHYS_TO_VIRT_P(proc->stack_phys)-argvs);
                    size_t strptr = 0;
                    for (int j = 0; j < argc; j++){
                        size_t strsize = strlen(argv[j]);
                        memcpy(nargvals + strptr, argv[j], strsize);
                        nargvals[strptr + strsize] = 0;
                        nargv[j] = vnargvals + strptr;
                        strptr += strsize+1;
                    }
                    proc->PROC_X1 = (uintptr_t)proc->stack-argvs;
                    proc->sp = (proc->stack - total) & ~0xFULL;
                } else {
                    proc->PROC_X0 = 0;
                }
            }
            proc->state = READY;
            release(full_name);
            return proc;
        }
        release(full_name);
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

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
    size_t listsize = 0x1000;
    void *listptr = zalloc(listsize);
    char *full_name = 0;

    if (listptr && list_directory_contents("/boot/redos/bin/", listptr, listsize, 0)){
        full_name = strcat_new(prog_name, ".elf");
        string_list *list = (string_list*)listptr;
        char* reader = (char*)list->array;
        kprintf("Directory contains %i files",list->count);
        for (uint32_t i = 0; i < list->count; i++){
            char *f = reader;
            while (*reader) reader++;
            reader++;

            if (!*f) continue;
            if (!full_name) continue;
            if (strcmp_case(f, full_name,true) != 0) continue;

            string path = string_format("/boot/redos/bin/%s",full_name);
            file fd = {};
            FS_RESULT op = openf(path.data, &fd);
            if (op != FS_RESULT_SUCCESS){
                kprintf("Failed to open file %s",path.data);
                string_free(path);
                release(full_name);
                release(listptr);
                return 0;
            }
            char *program = zalloc(fd.size);
            if (!program){
                kprintf("Failed to read file %s", path.data);
                closef(&fd);
                string_free(path);
                release(full_name);
                release(listptr);
                return 0;
            }
            if (readf(&fd, program, fd.size) != fd.size){
                kprintf("Failed to read file %s", path.data);
            }
            process_t *proc = load_elf_file(prog_name, 0, program, fd.size);
            closef(&fd);
            release(program);
            string_free(path);
            if (!proc){
                kprintf("Failed to create process for %s",prog_name);
                release(full_name);
                release(listptr);
                return 0;
            }
            proc->PROC_X0 = argc;
            if (argc > 0){
                uintptr_t start = (uintptr_t)argv[0];
                uintptr_t end = (uintptr_t)argv;
                size_t total = end-start;
                size_t argvs = argc * sizeof(uintptr_t);
                char *nargvals = (char*)(PHYS_TO_VIRT_P(proc->stack_phys)-total-argvs);
                char *vnargvals = (char*)(proc->stack-total-argvs);
                char** nargv = (char**)(PHYS_TO_VIRT_P(proc->stack_phys)-argvs);
                uintptr_t strptr = 0;
                for (int j = 0; j < argc; j++){
                    size_t strsize = strlen(argv[j]);
                    memcpy(nargvals + strptr, argv[j], strsize);
                    *(char*)(nargvals + strptr + strsize++) = 0;
                    nargv[j] = vnargvals + strptr;
                    strptr += strsize;
                }
                proc->PROC_X1 = (uintptr_t)proc->stack-argvs;
                proc->sp -= total+argvs;
            }
            proc->state = READY;
            release(full_name);
            release(listptr);
            return proc;
        }
    }
    if (full_name) release(full_name);
    if (listptr) release(listptr);

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

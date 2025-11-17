#include "bin_mod.h"
#include "ping.h"
#include "tracert.h"
#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscalls.h"
#include "console/kio.h"
#include "process/loading/elf_file.h"
#include "memory/page_allocator.h"
#include "std/memory.h"

bool init_bin(){
    return true;
}

typedef struct open_bin_ref {
    char *name;
    int (*func)(int argc, char* argv[]);
} open_bin_ref;

open_bin_ref available_cmds[] = {
    { "ping", run_ping },
    { "tracert", run_tracert },
};

process_t* execute(const char* prog_name, int argc, const char* argv[]){

    size_t listsize = 0x1000;
    void *listptr = malloc(listsize);
    if (list_directory_contents("/boot/redos/bin/", listptr, listsize, 0)){
        size_t name_len = strlen(prog_name,0) + 4;
        char *full_name = (char*)malloc(name_len);
        strcat(prog_name, ".elf", full_name);
        string_list *list = (string_list*)listptr;
        char* reader = (char*)list->array;
        kprintf("Directory contains %i files",list->count);
        for (uint32_t i = 0; i < list->count; i++){
            char *f = reader;
            if (*f){
                if (strcmp(f, full_name, true) == 0){
                    string path = string_format("/boot/redos/bin/%s",full_name);
                    file fd = {};
                    FS_RESULT op = fopen(path.data, &fd);
                    if (op != FS_RESULT_SUCCESS){
                        kprintf("Failed to open file %s",path.data);
                        return 0;
                    }
                    char *program = malloc(fd.size);
                    if (fread(&fd, program, fd.size) != fd.size){
                        kprintf("Failed to read file %s", path.data);
                    }
                    process_t *proc = load_elf_file(prog_name, 0, program, fd.size);
                    string_free(path);
                    free(full_name,name_len);
                    if (!proc){
                        kprintf("Failed to create process for %s",prog_name);
                    }
                    proc->PROC_X0 = argc;
                    size_t total = 0;
                    for (int i = 0; i < argc; i++)
                        total += strlen(argv[i], 0);
                    char **nargv = (char **)kalloc((void*)proc->heap, total, ALIGN_16B, MEM_PRIV_USER);
                    memcpy(nargv, argv, total);
                    proc->PROC_X1 = (uintptr_t)nargv;
                    proc->state = READY;
                    return proc;
                }
                while (*reader) reader++;
                reader++;
            }
        }
        free(full_name,name_len);
        //TODO: The list of strings needs to be freed, but this class is not its owner
    }

    for (uint32_t i = 0; i < N_ARR(available_cmds); i++){
        if (strcmp(available_cmds[i].name, prog_name, false) == 0){
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

driver_module bin_module = (driver_module){
    .name = "bin",
    .mount = "/bin",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_bin,
    .fini = 0,
    .open = 0,
    .read = read_bin,
    .write = 0,
    .sread = 0,
    .swrite = 0,
    .readdir = list_bin,
};
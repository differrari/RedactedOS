#include "bin_mod.h"
#include "cat.h"
#include "kernel_processes/kprocess_loader.h"

bool init_bin(){
    return true;
}

typedef struct open_bin_ref {
    char *name;
    int (*func)(int argc, char* argv[]);
} open_bin_ref;

open_bin_ref available_cmds[] = {
    { "cat", run_cat }
};

process_t* execute(const char* prog_name, int argc, const char* argv[]){
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

sizedptr list_bin(const char *path){
    return (sizedptr){0,0};
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
    .seek = 0,
    .readdir = list_bin,
};
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
#include "std/memory.h"
#include "process/scheduler.h"
#include "sysregs.h"
#include "input/input_dispatch.h"

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

process_t* execute(const char* prog_name, int argc, const char* argv[], uint32_t mode){
    if (!prog_name || !*prog_name) return 0;
    if (mode != EXEC_MODE_KEEP_FOCUS) mode = EXEC_MODE_DEFAULT;

    process_t *cur = get_current_proc();
    uint16_t win_id = cur ? cur->win_id : 0;
    bool transfer_focus = win_id && mode == EXEC_MODE_DEFAULT;

    if (strcont(prog_name, "/")){
        const char *name = prog_name;
        for (const char *p = prog_name; *p; p++) if (*p == '/') name = p + 1;

        char proc_name[256] = {};
        size_t i = 0;
        while (name[i] && name[i] != '.' && i + 1 < sizeof(proc_name)){
            proc_name[i] = name[i];
            i++;
        }

        string bundle = string_from_literal_length(prog_name, name - prog_name - 1);
        process_t *proc = load_elf_process_path(proc_name, bundle.data, prog_name, argc, argv);
        release(bundle.data);
        if (!proc) return 0;

        if (win_id) proc->win_id = win_id;
        if (transfer_focus) sys_set_focus(proc->id);
        return proc;
    }

    char *full_name = (strend_case(prog_name, ".elf", true) == 0) ? string_from_literal(prog_name).data : strcat_new(prog_name, ".elf");
    if (full_name) {
        char pathbuf[1024] = {};
        size_t pathlen = string_format_buf(pathbuf, sizeof(pathbuf), "/boot/redos/bin/%s",full_name);
        process_t *proc = 0;
        if (pathlen < sizeof(pathbuf) - 1) proc = load_elf_process_path(prog_name, 0, pathbuf, argc, argv);
        release(full_name);
        if (proc) {
            if (win_id) proc->win_id = win_id;
            if (transfer_focus) sys_set_focus(proc->id);
            return proc;
        }
    }

    for (uint32_t i = 0; i < N_ARR(available_cmds); i++){
        if (strcmp(available_cmds[i].name, prog_name) == 0){
            process_t *proc = create_kernel_process(available_cmds[i].name, available_cmds[i].func, argc, argv);
            if (!proc) return 0;
            if (win_id) proc->win_id = win_id;
            if (transfer_focus) sys_set_focus(proc->id);
            return proc;
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
    .mount = "bin",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_bin,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .readdir = 0,
};

#include "tools.h"
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

bool init_tools(){
    return true;
}

typedef struct open_tools_ref {
    char *name;
    int (*func)(int argc, char* argv[]);
} open_tools_ref;

open_tools_ref available_cmds[] = {
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
    bool transfer_focus = mode == EXEC_MODE_DEFAULT;
   
    if (strcont(prog_name, "/")){
        const char *name = prog_name;
        for (const char *p = prog_name; *p; p++) if (*p == '/') name = p + 1;

        char proc_name[256] = {};
        size_t i = 0;
        while (name[i] && name[i] != '.' && i + 1 < sizeof(proc_name)){
            proc_name[i] = name[i];
            i++;
        }

        process_t *proc = load_elf_process_path(proc_name, prog_name, prog_name, argc, argv);
        if (!proc) {
            //TODO: this should only be for .red
            string executable = string_format("%s/%s.elf",prog_name,proc_name);
            proc = load_elf_process_path(proc_name, prog_name, executable.data, argc, argv);
            release(executable.data);
            if (!proc) return 0;
        }

        if (win_id) proc->win_id = win_id;
        if (transfer_focus) sys_set_focus(proc->id);
        return proc;
    }

    
    char pathbuf[1024];
    string_format_buf(pathbuf, sizeof(pathbuf), "/boot/redos/tools/%s",prog_name);
    process_t *proc = load_elf_process_path(prog_name, 0, pathbuf, argc, argv);
    if (!proc){
        string_format_buf(pathbuf, sizeof(pathbuf), "/home/tools/%s",prog_name);
        proc = load_elf_process_path(prog_name, 0, pathbuf, argc, argv);
    }
    if (proc) {
        if (win_id) proc->win_id = win_id;
        if (transfer_focus) sys_set_focus(proc->id);
        return proc;
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

size_t ktools_list(const char *path, void *buf, size_t size, file_offset *offset){
    file_offset off = offset ? *offset : 0;
    fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    for (u64 i = off; i < N_ARR(available_cmds); i++){
        if (!dir_list_fill(&helper, available_cmds[i].name)){
            if (offset) *offset = i;
            return dir_buf_size(&helper);
        }
    }
    return dir_buf_size(&helper);
}

bool ktools_stat(const char *path, fs_stat *out_stat){
    if (strlen(path) && *path == '/') path++;
    if (!strlen(path)){
        stat_dir(out_stat);
        return true;
    }
    for (u64 i = 0; i < N_ARR(available_cmds); i++){
        if (strncmp(available_cmds[i].name, path, strlen(available_cmds[i].name)) == 0){
            out_stat->size = 0;
            out_stat->data_type = DATA_SIGNATURE("KPROC");
            out_stat->type = entry_file;
            return true;
        }
    }
    return true;
}

//TODO: finish listing tool module
system_module tool_module = (system_module){
    .name = "tools",
    .mount = "ktools",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_tools,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .getstat = ktools_stat,
    .readdir = ktools_list,
};

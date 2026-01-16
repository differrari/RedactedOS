#include "shutdown.h"

#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "std/string.h"
#include "hw/power.h"
#include "syscalls/syscalls.h"

int run_shutdown(int argc, char* argv[]){
    const char *u = "usage: shutdown [-r|-p]\n  -r  reboot\n  -p  power off\n";


    uint16_t pid = get_current_proc_pid();
    string p = string_format("/proc/%i/out", pid);
    file out;
    FS_RESULT r = open_file(p.data, &out);
    free_sized(p.data, p.mem_length);

    if (r != FS_RESULT_SUCCESS){
        return 2;
    }
    if (argc <= 0){
        write_file(&out, u,strlen(u));
         return 0;
    }

    int mode = -1;

    for (int i = 0; i < argc; ++i){
        const char *a = argv[i];
        if (!a || a[0] == 0) continue;

        if (strcmp(a, "-r") == 0) mode = SHUTDOWN_REBOOT;
        else if (strcmp(a, "-p") == 0) mode = SHUTDOWN_POWEROFF;
        else{
            write_file(&out, u,strlen(u));
            msleep(100);
            close_file(&out);
            return 2;
        }
    }

    if (mode == -1){
        write_file(&out, u,strlen(u));
        msleep(100);
        close_file(&out);
        return 2;
    }

    if (mode == SHUTDOWN_REBOOT) write_file(&out, "Rebooting...\n", 13);
    else write_file(&out, "Powering off...\n", 16);

    msleep(100);
    close_file(&out);
    hw_shutdown(mode);
    return 0;
}
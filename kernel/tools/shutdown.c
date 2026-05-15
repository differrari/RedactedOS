#include "shutdown.h"

#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "std/string.h"
#include "hw/power.h"
#include "syscalls/syscalls.h"

int run_shutdown(int argc, char* argv[]){
    const char *u = "usage: shutdown [-r|-p]\n  -r  reboot\n  -p  power off\n";


    if (argc <= 0){
        print("%s", u);
        return 0;
    }

    int mode = -1;

    for (int i = 1; i < argc; ++i){
        const char *a = argv[i];
        if (!a || a[0] == 0) continue;

        if (strcmp(a, "-r") == 0) mode = SHUTDOWN_REBOOT;
        else if (strcmp(a, "-p") == 0) mode = SHUTDOWN_POWEROFF;
        else{
            print("%s", u);
            msleep(100);
            return 2;
        }
    }

    if (mode == -1){
        print("%s", u);
        msleep(100);
        return 2;
    }

    if (mode == SHUTDOWN_REBOOT) print("Rebooting...\n");
    else print("Powering off...\n");

    msleep(100);
    hw_shutdown(mode);
    return 0;
}
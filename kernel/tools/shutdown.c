#include "shutdown.h"

#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "std/string.h"
#include "hw/power.h"
#include "syscalls/syscalls.h"

static void print_help(void) {
    print("Usage:\t shutdown -r|-p");
    print("shut down or reboot\n");
    print("Options:");
    print("\t -r\t reboot");
    print("\t -p\t power off");
    print("\t --help\t help");
}

int run_shutdown(int argc, char* argv[]){
    if (argc <= 0){
        print_help();
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_help();
        return 0;
    }

    int mode = -1;

    for (int i = 1; i < argc; ++i){
        const char *a = argv[i];
        if (!a || a[0] == 0) continue;

        if (strcmp(a, "-r") == 0) mode = SHUTDOWN_REBOOT;
        else if (strcmp(a, "-p") == 0) mode = SHUTDOWN_POWEROFF;
        else{
            print_help();
            msleep(100);
            return 2;
        }
    }

    if (mode == -1){
        print_help();
        msleep(100);
        return 2;
    }

    if (mode == SHUTDOWN_REBOOT) print("Rebooting...\n");
    else print("Powering off...\n");

    msleep(100);
    hw_shutdown(mode);
    return 0;
}
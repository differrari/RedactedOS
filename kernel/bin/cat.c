#include "cat.h"
#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"

void run_cat(){
    const char* arg1 = "/proc/8/out";
    size_t arg2 = 100;
    file fd;
    open_file(arg1, &fd);
    if (fd.size == 0) stop_current_process();// 1;
    if (arg2 == 0) arg2 = fd.size;
    char* buf = (char*)malloc(arg2);
    read_file(&fd, buf, arg2);

    uint16_t pid = get_current_proc_pid();

    string s = string_format("/proc/%i/out",pid);
    file fd2;
    open_file(s.data, &fd2);
    write_file(&fd2, buf, arg2);
    free(s.data, s.mem_length);
    stop_current_process();
}

process_t* create_cat_process(const char *args){
    return create_kernel_process("cat", run_cat);
}
#include "cat.h"
#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "console/kio.h"

//TODO: Move to /os/bin
int run_cat(int argc, char* argv[]){
    uint16_t pid = get_current_proc_pid();
    string s = string_format("/proc/%i/out",pid);
    file fd2;
    open_file(s.data, &fd2);
    free(s.data, s.mem_length);
    string err_msg = string_from_literal("Deprecated cat. meow :(");
    write_file(&fd2, err_msg.data, err_msg.length);
    free(err_msg.data, err_msg.mem_length);
    return 2;
}
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
    if (argc != 2){
        string err_msg = string_from_literal("Usage cat <path> <size>");
        write_file(&fd2, err_msg.data, err_msg.length);
        free(err_msg.data, err_msg.mem_length);
        return 2;
    }
    const char* path = argv[0];
    size_t size = parse_int_u64(argv[1], UINT32_MAX);
    file fd;
    open_file(path, &fd);
    if (fd.size == 0){
        string err_msg = string_format("Couldn't find file %s", argv[0]);
        write_file(&fd2, err_msg.data, err_msg.length);
        free(err_msg.data, err_msg.mem_length);
        return 1;
    } 
    if (size == 0) size = fd.size;
    char* buf = (char*)malloc(size);
    read_file(&fd, buf, size);
    write_file(&fd2, buf, size);
    close_file(&fd);
    close_file(&fd2);
    return 0;
}
#include "cat.h"
#include "kernel_processes/kprocess_loader.h"
#include "filesystem/filesystem.h"
#include "process/scheduler.h"
#include "syscalls/syscalls.h"
#include "console/kio.h"

int run_cat(int argc, char* argv[]){
    if (argc != 2) return 2;
    kprintf("Cat with %i arguments. %s", argc, argv[0]);
    const char* path = argv[0];
    kprintf("Cat with %i arguments. %s", argc, argv[1]);
    size_t size = parse_int_u64(argv[1], UINT32_MAX);
    file fd;
    open_file(path, &fd);
    if (fd.size == 0) return 1;
    if (size == 0) size = fd.size;
    char* buf = (char*)malloc(size);
    read_file(&fd, buf, size);

    uint16_t pid = get_current_proc_pid();

    string s = string_format("/proc/%i/out",pid);
    file fd2;
    open_file(s.data, &fd2);
    write_file(&fd2, buf, size);
    free(s.data, s.mem_length);
    return 0;
}

process_t* create_cat_process(int argc, const char *argv[]){
    return create_kernel_process("cat", run_cat, argc, argv);
}
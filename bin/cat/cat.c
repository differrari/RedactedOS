#include "syscalls/syscalls.h"

int main(int argc, char* argv[]){
    // uint16_t pid = get_current_proc_pid();
    string s = string_format("/proc/%i/out",0);
    file fd2;
    fopen(s.data, &fd2);
    free(s.data, s.mem_length);
    if (argc != 2){
        string err_msg = string_from_literal("Usage cat <path> <size>");
        fwrite(&fd2, err_msg.data, err_msg.length);
        free(err_msg.data, err_msg.mem_length);
        return 2;
    }
    const char* path = argv[0];
    size_t size = parse_int_u64(argv[1], UINT32_MAX);
    file fd;
    fopen(path, &fd);
    if (fd.size == 0){
        string err_msg = string_format("Couldn't find file %s", argv[0]);
        fwrite(&fd2, err_msg.data, err_msg.length);
        free(err_msg.data, err_msg.mem_length);
        return 1;
    } 
    if (size == 0) size = fd.size;
    char* buf = (char*)malloc(size);
    fread(&fd, buf, size);
    fwrite(&fd2, buf, size);
    fclose(&fd);
    fclose(&fd2);
    return 0;
}
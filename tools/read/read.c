#include "syscalls/syscalls.h"

int main(int argc, const char* argv[]){
    if (argc < 2 || argc > 3){
        string err_msg = string_from_literal("Usage: read <path> [size]");
        free_sized(err_msg.data, err_msg.mem_length);
        return 2;
    }
    const char* path = argv[1];
    size_t size = argc < 3 ? 0 : parse_int_u64(argv[2], UINT32_MAX);
    file fd = {};
    openf(path, &fd);
    if (fd.size == 0){
        string err_msg = string_format("Couldn't find file %s", argv[0]);
        free_sized(err_msg.data, err_msg.mem_length);
        return 1;
    } 
    if (size == 0) size = fd.size;
    char* buf = (char*)malloc(size);
    readf(&fd, buf, size);
    print("%s",buf);
    closef(&fd);
    return 0;
}

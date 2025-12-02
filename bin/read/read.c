#include "syscalls/syscalls.h"

int main(int argc, const char* argv[]){
    file fd2 = { .id = 2 };
    if (argc < 1 || argc > 2){
        string err_msg = string_from_literal("Usage: read <path> [size]");
        fwrite(&fd2, err_msg.data, err_msg.length);
        free_sized(err_msg.data, err_msg.mem_length);
        return 2;
    }
    const char* path = argv[0];
    size_t size = argc < 2 ? 0 : parse_int_u64(argv[1], UINT32_MAX);
    file fd = {};
    fopen(path, &fd);
    if (fd.size == 0){
        string err_msg = string_format("Couldn't find file %s", argv[0]);
        fwrite(&fd2, err_msg.data, err_msg.length);
        free_sized(err_msg.data, err_msg.mem_length);
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
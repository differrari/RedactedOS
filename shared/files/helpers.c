#include "helpers.h"
#include "syscalls/syscalls.h"

#ifndef CROSS

void traverse_directory(char *directory, bool recursive, dir_traverse func){
    print("[FS implementation error] Traverse directory not implemented for [REDACTED]");
}

char* get_current_dir(){
    print("[FS implementation error] Current directory not implemented for [REDACTED]");
    return 0;
}

char* gethomedir(){
    print("[FS implementation error] Home directory not implemented for [REDACTED]");
    return 0;
}

#endif
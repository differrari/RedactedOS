#include "syscalls.h"
#include "std/string.h"
#include "math/math.h"
#include "std/memory.h"

static char log_buf[1024];

int print(const char *fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt); 
    size_t n = string_format_va_buf(fmt, log_buf, sizeof(log_buf), args);
    va_end(args);
    if (n >= sizeof(log_buf)) log_buf[sizeof(log_buf)-1] = '\0';
    printl(log_buf);
#ifndef CROSS
    file fd2 = { .id = 2 };
    writef(&fd2, log_buf, strlen(log_buf));
#endif
    return 0;
}

int printf(const char *fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    char li[256]; 
    size_t n = string_format_va_buf(fmt, li, sizeof(li), args);
    va_end(args);
    if (n >= sizeof(li)) li[sizeof(li)-1] = '\0';
    printl(li);
#ifndef CROSS
    file fd2 = { .id = 2 };
    writef(&fd2, li, strlen(li));
#endif
    return 0;
}

void seek(file *descriptor, int64_t offset, SEEK_TYPE type){
    uint64_t new_cursor = descriptor->cursor;
    switch (type) {
        case SEEK_ABSOLUTE:
            new_cursor = (uint64_t)offset;
            break;
        case SEEK_RELATIVE:
            new_cursor += offset;
            break;
    }
    if (new_cursor > descriptor->size) return;//TODO: check what happens if we intentionally mess with the descriptor size before changing
    descriptor->cursor = new_cursor;
}

void* realloc_sized(void* old_ptr, size_t old_size, size_t new_size){
    void* new_ptr = malloc(new_size);
    memcpy(new_ptr, old_ptr, min(old_size,new_size));
    free_sized(old_ptr, old_size);
    return new_ptr;
}

#ifndef CROSS

void begin_drawing(draw_ctx *ctx){

}

void destroy_draw_ctx(draw_ctx *ctx){

}

bool should_close_ctx(){
    return false;
}

void* calloc(size_t nitems, size_t size){
    return malloc(nitems * size);
}

void* zalloc(size_t size){
    return malloc(size);
}

char *read_full_file(const char *path){
    
    file fd = {};
    if (openf(path, &fd) != FS_RESULT_SUCCESS) return false;
    char *fcontent = (char*)malloc(fd.size + 1);
    
    readf(&fd, fcontent, fd.size);
    
    closef(&fd);
    
    return fcontent;
}

bool write_full_file(const char *path, void* buf, size_t size){
    file fd = {};
    if (openf(path, &fd) != FS_RESULT_SUCCESS) return false;
    
    size_t res = writef(&fd, buf, size);
    
    closef(&fd);
    
    return res > 0;
}

int system(const char *command){
    print("[SYS implementation error] system not implemented");
    return -1;
}

#endif

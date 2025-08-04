#include "kio.h"
#include "serial/uart.h"
#include "kconsole/kconsole.h"
#include "std/string.h"
#include "memory/page_allocator.h"
#include "std/memfunctions.h"
#include "math/math.h"

static bool use_visual = true;
char* print_buf;
uintptr_t cursor;

#define CONSOLE_BUF_SIZE 0x3000

void reset_buffer(){
    cursor = (uintptr_t)print_buf;
    memset(print_buf, 0, CONSOLE_BUF_SIZE);
}


void init_print_buf(){
    print_buf = palloc(CONSOLE_BUF_SIZE,true, false, true);
    reset_buffer();
}

bool console_init(){
    enable_uart();
    return true;
}

bool console_fini(){
    return false;
}

FS_RESULT console_open(const char *path, file *out_fd){
    out_fd->id = reserve_fd_id();
    out_fd->size = CONSOLE_BUF_SIZE;
    return FS_RESULT_SUCCESS;
}

size_t console_read(file *fd, char *out_buf, size_t size, file_offset offset){
    memcpy(out_buf, print_buf+offset, min(size,CONSOLE_BUF_SIZE));
    return 0;
}

size_t console_write(file *fd, const char *buf, size_t size, file_offset offset){
    //TODO: kinda allowing arbitrary buffers here
    kprintf(buf);
    return size;
}

file_offset console_seek(file *fd, file_offset offset){
    return 0;
}

sizedptr console_readdir(const char* path){
    return (sizedptr){ 0, 0 };
}

driver_module console_module = (driver_module){
    .name = "console",
    .mount = "/dev/console",
    .version = VERSION_NUM(0,1,0,0),
    .init = console_init,
    .fini = console_fini,
    .open = console_open,
    .read = console_read,
    .write = console_write,
    .seek = console_seek,
    .readdir = console_readdir,
};

void puts(const char *s){
    uart_raw_puts(s);
    if (use_visual)
        kconsole_puts(s);
}

void putc(const char c){
    uart_raw_putc(c);
    if (use_visual)
        kconsole_putc(c);
}

void kprintf(const char *fmt, ...){
    if (!print_buf) init_print_buf();
    va_list args;
    va_start(args, fmt);

    //TODO: If we don't read this value, the logs crash. Could it be stack overflow?
    mem_page *info = (mem_page*)print_buf;
    
    if (cursor >= ((uintptr_t)print_buf) + 0x2F00){
        reset_buffer();
    }

    //TODO: string_format_va_buf should be given a maximum size
    size_t len = string_format_va_buf(fmt, (char*)cursor, args);
    va_end(args);
    puts((char*)cursor);
    putc('\r');
    putc('\n');
    cursor += len;
    *(char*)(cursor++) = '\r';
    *(char*)(cursor++) = '\n';
}

void kprint(const char *fmt){
    puts(fmt);
    putc('\r');
    putc('\n');

     while (*fmt != '\0') {
        *(char*)(cursor++) = *fmt;
        fmt++;
    }
    *(char*)(cursor++) = '\r';
    *(char*)(cursor++) = '\n';
}

void kputf(const char *fmt, ...){
    if (!print_buf) init_print_buf();
    va_list args;
    va_start(args, fmt);

    if (cursor >= ((uintptr_t)print_buf) + CONSOLE_BUF_SIZE - 0x100){
        reset_buffer();
    }

    size_t len = string_format_va_buf(fmt, (char*)cursor, args);
    va_end(args);
    puts((char*)cursor);
    cursor += len-1;
}

void disable_visual(){
    use_visual = false;
    kconsole_clear();
}

void enable_visual(){
    use_visual = true;
}
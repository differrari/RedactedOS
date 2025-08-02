#include "kio.h"
#include "serial/uart.h"
#include "kconsole/kconsole.h"
#include "std/string.h"
#include "memory/page_allocator.h"

static bool use_visual = true;
void* print_buf;

bool console_init(){
    enable_uart();
    return true;
}

bool console_fini(){
    return false;
}

FS_RESULT console_open(const char *path, file *out_fd){
    return FS_RESULT_SUCCESS;
}

size_t console_read(file *fd, char *out_buf, size_t size, file_offset offset){
    return 0;
}

size_t console_write(file *fd, const char *buf, size_t size, file_offset offset){
    kprintf(buf);
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

void init_print_buf(){
    print_buf = palloc(0x1000,true, false, false);
}

void kprintf(const char *fmt, ...){
    if (!print_buf) init_print_buf();
    va_list args;
    va_start(args, fmt);
    char* buf = kalloc(print_buf, 256, ALIGN_64B, true, false);
    size_t len = string_format_va_buf(fmt, buf, args);
    va_end(args);
    puts(buf);
    putc('\r');
    putc('\n');
    kfree((void*)buf, 256);
}

void kprint(const char *fmt){
    puts(fmt);
    putc('\r');
    putc('\n');
}

void kputf(const char *fmt, ...){
    if (!print_buf) init_print_buf();
    va_list args;
    va_start(args, fmt);
    char* buf = kalloc(print_buf, 256, ALIGN_64B, true, false);
    size_t len = string_format_va_buf(fmt, buf, args);
    va_end(args);
    puts(buf);
    kfree((void*)buf, 256);
}

void disable_visual(){
    use_visual = false;
}

void enable_visual(){
    use_visual = true;
}
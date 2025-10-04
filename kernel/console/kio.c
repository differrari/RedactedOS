#include "kio.h"
#include "serial/uart.h"
#include "kconsole/kconsole.h"
#include "std/string.h"
#include "memory/page_allocator.h"
#include "std/memory.h"
#include "math/math.h"
#include "data_struct/ring_buffer.h"

static bool use_visual = true;

#define CONSOLE_BUF_SIZE 0x3000
#define CONSOLE_WRITE_CHUNK 256

static CRingBuffer console_rb;
static uint8_t *console_storage;
static volatile uint64_t console_drop_count;

static void console_out_crlf(){
    uart_raw_putc('\r');
    uart_raw_putc('\n');
    if (use_visual) {
        kconsole_putc('\r');
        kconsole_putc('\n');
    }
}

static void console_ring_write(const char *src,size_t n){
    for (size_t i = 0; i < n; i++) {
        if (cring_push(&console_rb, &src[i]) < 0) {
            console_drop_count++;
        }
    }
}

static void init_print_buf(){
    if (!console_storage) {
        console_storage = palloc(CONSOLE_BUF_SIZE,MEM_PRIV_KERNEL, MEM_RW, true);
        cring_init(&console_rb, console_storage, CONSOLE_BUF_SIZE, 1);
        console_drop_count = 0;
    }
}

bool console_init(){
    init_print_buf();
    enable_uart();
    kprint("UART initialized");
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
    if (!console_storage) init_print_buf();

    uint64_t off = (uint64_t)offset;
    uint64_t sz = cring_capacity(&console_rb);
    uint64_t head = console_rb.head;
    uint64_t tail = console_rb.tail;
    uint64_t avail = console_rb.full ? sz : (head >= tail ? head - tail : sz - (tail - head));

    if (off >= avail) return 0;

    uint64_t start = console_rb.full ? head : tail; 
    uint64_t base = (start + off) % sz;

    size_t to_read = size;
    if (to_read > CONSOLE_BUF_SIZE) to_read = CONSOLE_BUF_SIZE;
    if (to_read > (size_t)(avail- off)) to_read = (size_t)(avail - off);

    for (size_t i = 0; i < to_read; i++) {
        out_buf[i] = ((uint8_t *)console_rb.buffer)[(base + i) % sz];
    }
    return to_read;
}

size_t console_write(file *fd, const char *buf, size_t size, file_offset offset){
    if (!console_storage) init_print_buf();
    if (!buf || size == 0) return 0;

    size_t i = 0;
    while (i < size) {
        size_t take = size - i;
        if (take > CONSOLE_WRITE_CHUNK) take = CONSOLE_WRITE_CHUNK;

        char tmp[CONSOLE_WRITE_CHUNK + 1];
        for (size_t j = 0; j < take; j++) tmp[j] = buf[i + j];
        tmp[take] = '\0';

        uart_raw_puts(tmp);
        if (use_visual) kconsole_puts(tmp);

        i += take;
    }

    console_ring_write(buf, size);
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
    .version = VERSION_NUM(0,1,1,0),
    .init = console_init,
    .fini = console_fini,
    .open = console_open,
    .read = console_read,
    .write = console_write,
    .seek = console_seek,
    .readdir = console_readdir,
};

void puts(const char *s){
    if (!console_storage) init_print_buf();
    if (!s) return;

    size_t n = strlen(s, 0);
    if (n) console_write(NULL, s, n, 0);
}

void putc(const char c){
    if (!console_storage) init_print_buf();
    char t[1] = {c};
    console_write(NULL, t, 1, 0);
}

void kprintf(const char *fmt, ...){
    if (!console_storage) init_print_buf();
    if (!fmt) return;

    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);

    char buf[STRING_MAX_LEN];
    size_t n = string_format_va_buf(fmt, buf, STRING_MAX_LEN, args);

    va_end(args);
    console_write(NULL, buf, n, 0);
    console_out_crlf();
    const char crlf[2] = {'\r','\n'};
    console_ring_write(crlf, 2);
}

void kprint(const char *s){
    if (!console_storage) init_print_buf();
    if (!s) return;

    size_t n = strlen(s, 0);
    if (n) console_write(NULL, s, n, 0);

    console_out_crlf();

    const char crlf[2] = {'\r', '\n'};
    console_ring_write(crlf, 2);
}

void kputf(const char *fmt, ...){
    if (!console_storage) init_print_buf();
    if (!fmt) return;

    va_list args;
    va_start(args, fmt);

    char buf[STRING_MAX_LEN];
    size_t n = string_format_va_buf(fmt, buf, STRING_MAX_LEN, args);

    va_end(args);

    console_write(NULL, buf, n, 0);
}

void disable_visual(){
    use_visual = false;
    kconsole_clear();
}

void enable_visual(){
    use_visual = true;
}
#include "kio.h"
#include "serial/uart.h"
#include "kconsole/kconsole.h"
#include "std/string.h"
#include "memory/page_allocator.h"
#include "std/memory.h"
#include "math/math.h"
#include "data_struct/ring_buffer.h"

static bool use_visual = false;

#define CONSOLE_BUF_SIZE 0x3000
#define CONSOLE_WRITE_CHUNK 256

#define CR '\r'
#define LF '\n'
static const char CRLF[2] = { CR, LF };

static CRingBuffer console_rb;
static uint8_t *console_storage;
static volatile uint64_t console_drop_count;

static void console_out_crlf(){
    uart_raw_putc(CR);
    uart_raw_putc(LF);
    if (use_visual) {
        kconsole_putc(CR);
        kconsole_putc(LF);
    }
}

static void console_ring_write(const char *src, size_t n) {
    size_t i = 0;

    while (i < n) {
        const uint64_t sz = cring_capacity(&console_rb);
        const uint64_t head = console_rb.head;
        const uint64_t tail = console_rb.tail;
        const bool full = console_rb.full;

        const uint64_t used = full ? sz : (head >= tail ? (head - tail) : (sz - (tail - head)));
        const uint64_t free = sz - used;

        if (free == 0) {
            break;
        }

        const size_t chunk = (size_t)((n - i) < free ? (n - i) : free);

        for (size_t k = 0; k < chunk; ++k) {
            cring_push(&console_rb, &src[i + k]);
        }

        i += chunk;
    }

    if (i < n) {
        console_drop_count += (uint64_t)(n - i);
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
    out_fd->id = reserve_fd_gid("/console");
    out_fd->size = CONSOLE_BUF_SIZE;
    return FS_RESULT_SUCCESS;
}

size_t console_read(file *fd, char *out_buf, size_t size, file_offset offset){
    if (!console_storage) init_print_buf();
    if (!out_buf || size == 0) return 0;

    const uint64_t off = (uint64_t)offset;
    const uint64_t sz = cring_capacity(&console_rb);
    const uint64_t head = console_rb.head;
    const uint64_t tail = console_rb.tail;
    const bool full = console_rb.full;

    const uint64_t avail = full ? sz : (head >= tail ? head - tail : sz - (tail - head));
    if (off >= avail) return 0;

    const uint64_t start_oldest = full ? head : tail;

    size_t to_read = size;
    if (to_read > CONSOLE_BUF_SIZE) to_read = CONSOLE_BUF_SIZE;

    const uint64_t rem = avail - off;
    if (to_read > rem) to_read = (size_t)rem;

    const uint64_t base = (start_oldest + (rem - to_read)) % sz;

    uint8_t *basep = (uint8_t*)console_rb.buffer;

    const size_t first = (size_t)min((uint64_t)to_read, sz - base);
    memcpy(out_buf, basep + base, first);
    if (first < to_read) memcpy(out_buf + first, basep, to_read - first);

    return to_read;
}

size_t console_write(const char *buf, size_t size){
    if (!console_storage) init_print_buf();
    if (!buf || size == 0) return 0;

    for (size_t i = 0; i < size;) {
        const size_t remain = size - i;
        const size_t take = remain > CONSOLE_WRITE_CHUNK ? CONSOLE_WRITE_CHUNK : remain;
        const char *chunk = buf + i;


        size_t k = 0;
        while (k < take){
            size_t run = 0;
            while (k + run < take && chunk[k + run] != '\0') run++;

            if (run) {
                char tmp[CONSOLE_WRITE_CHUNK + 1];
                memcpy(tmp, chunk + k, run);
                tmp[run] = '\0';

                uart_raw_puts(tmp);
                if (use_visual) kconsole_puts(tmp);
                k += run;
            }
            if (k < take && chunk[k] == '\0') {
                k++;
            }
        }

        i += take;
    }

    console_ring_write(buf, size);
    return size;
}

size_t console_write_fd(file *fd, const char *buf, size_t size, file_offset offset){
    return console_write(buf, size);
}

size_t simple_console_write(const char *path, const void *buf, size_t size){
    return console_write(buf, size);
}

file_offset console_seek(file *fd, file_offset offset){
    return 0;
}

system_module console_module = (system_module){
    .name = "console",
    .mount = "/console",
    .version = VERSION_NUM(0,1,0,0),
    .init = console_init,
    .fini = console_fini,
    .open = console_open,
    .close = 0,
    .read = console_read,
    .write = console_write_fd,
    .sread = 0,//TODO implement simple io
    .swrite = simple_console_write,
    .readdir = 0,
};

void puts(const char *s){
    if (!console_storage) init_print_buf();
    if (!s) return;

    size_t n = strlen(s);
    if (!n) return;

    uart_raw_puts(s);
    if (use_visual) kconsole_puts(s);
    console_ring_write(s, n);
}

void putc(const char c){
    if (!console_storage) init_print_buf();

    uart_raw_putc(c);
    if (use_visual) kconsole_putc(c);
    console_ring_write(&c, 1);
}

void kprintf(const char *fmt, ...){
    if (!console_storage) init_print_buf();
    if (!fmt) return;

    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);

    char buf[STRING_MAX_LEN];
    size_t n = string_format_va_buf(fmt, buf, STRING_MAX_LEN, args);

    va_end(args);
    console_write(buf, n);
    console_out_crlf();
    console_ring_write(CRLF, 2);
}

void kprint(const char *s){
    if (!console_storage) init_print_buf();
    if (!s) return;

    size_t n = strlen(s);
    if (n) console_write(s, n);

    console_out_crlf();
    console_ring_write(CRLF, 2);
}

void kputf(const char *fmt, ...){
    if (!console_storage) init_print_buf();
    if (!fmt) return;

    va_list args;
    va_start(args, fmt);

    char buf[STRING_MAX_LEN];
    size_t n = string_format_va_buf(fmt, buf, STRING_MAX_LEN, args);

    va_end(args);

    console_write(buf, n);
}

void disable_visual(){
    use_visual = false;
    kconsole_clear();
}

void enable_visual(){
    use_visual = true;
}

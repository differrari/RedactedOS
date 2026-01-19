#include "buffer.h"
#include "syscalls/syscalls.h"

buffer buffer_create(size_t size, bool can_grow, bool circular){
    return (buffer){
        .buffer = zalloc(size),
        .buffer_size = 0,
        .limit = size,
        .can_grow = can_grow,
        .circular = circular,
        .cursor = 0,
    };
}

void buffer_write(buffer *buf, char* fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt); 
    size_t n = string_format_va_buf(fmt, buf->buffer+buf->cursor, buf->limit-buf->cursor, args);
    buf->cursor += n;
    buf->buffer_size += n;
    if (buf->can_grow && buf->buffer_size > buf->limit-256){
        size_t new_size = buf->limit;
        buf->buffer = realloc_sized(buf->buffer, buf->limit, new_size);
        buf->limit = new_size;
    }
    //TODO: circular
    va_end(args);
}

void buffer_write_space(buffer *buf){
    buffer_write(buf, " ");
}
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
    buffer_write_va(buf, fmt, args);
    //TODO: circular
    va_end(args);
}

void buffer_write_va(buffer *buf, char* fmt, va_list args){
    size_t n = string_format_va_buf(fmt, buf->buffer+buf->cursor, buf->limit-buf->cursor, args);
    buf->cursor += n;
    buf->buffer_size += n;
    if (buf->can_grow && buf->buffer_size > buf->limit-256){
        size_t new_size = buf->limit * 2;
        buf->buffer = realloc_sized(buf->buffer, buf->limit, new_size);
        buf->limit = new_size;
    }
}

void buffer_write_const(buffer *buf, char *lit){
    size_t lit_size = strlen(lit);
    buffer_write_lim(buf, lit, lit_size);
}

void buffer_write_lim(buffer *buf, char *lit, size_t lit_size){
    if (buf->limit - buf->buffer_size <= lit_size){
        if (buf->can_grow){
            size_t new_size = buf->limit;
            buf->buffer = realloc_sized(buf->buffer, buf->limit, new_size);
            buf->limit = new_size;
        } else return;
        //TODO: circular
    }
    for (size_t i = 0; i < lit_size; i++){
        buf->buffer[buf->cursor++] = lit[i];
    }
    buf->buffer_size += lit_size;
}

void buffer_write_space(buffer *buf){
    buffer_write_const(buf, " ");
}
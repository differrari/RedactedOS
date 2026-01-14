#pragma once

#include "types.h"
#include "ui/graphic_types.h"
#include "keyboard_input.h"
#include "mouse_input.h"
#include "std/string.h"
#include "net/network_types.h"
#include "ui/draw/draw.h"
#include "files/fs.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void printl(const char *str);

extern void* malloc(size_t size);
void* zalloc(size_t size);
extern void free_sized(void *ptr, size_t size);

extern bool read_key(keypress *kp);
extern bool read_event(kbd_event *event);
extern void get_mouse_status(mouse_data *in);

extern void msleep(uint64_t time);
extern void halt(int32_t exit_code);
extern uint16_t exec(const char* prog_name, int argc, const char* argv[]);

extern void request_draw_ctx(draw_ctx*);
extern void begin_drawing(draw_ctx *);
extern void commit_draw_ctx(draw_ctx*);
extern void resize_draw_ctx(draw_ctx*, uint32_t width, uint32_t height);
extern void destroy_draw_ctx(draw_ctx *ctx);

extern bool should_close_ctx();

extern uint32_t gpu_char_size(uint32_t scale);

extern uint64_t get_time();

extern bool socket_create(Socket_Role role, protocol_t protocol, const SocketExtraOptions* extra, SocketHandle *out_handle);
extern int32_t socket_bind(SocketHandle *handle, ip_version_t ip_version, uint16_t port);
extern int32_t socket_connect(SocketHandle *handle, SockDstKind dst_kind, void* dst, uint16_t port);
extern int32_t socket_listen(SocketHandle *handle);
extern bool socket_accept(SocketHandle *spec);
extern size_t socket_send(SocketHandle *handle, SockDstKind dst_kind, const void* dst, uint16_t port, void *packet, size_t size);
extern bool socket_receive(SocketHandle *handle, void *packet, size_t size, net_l4_endpoint* out_src);
extern int32_t socket_close(SocketHandle *handle);

int printf(const char *fmt, ...);

extern FS_RESULT openf(const char* path, file* descriptor);
extern size_t readf(file *descriptor, char* buf, size_t size);
extern size_t writef(file *descriptor, const char* buf, size_t size);
extern size_t sreadf(const char* path, void* buf, size_t size);
extern size_t swritef(const char* path, const void* buf, size_t size);
extern void closef(file *descriptor);
void seek(file *descriptor, int64_t offset, SEEK_TYPE type);
void* realloc_sized(void* old_ptr, size_t old_size, size_t new_size);

void *calloc(size_t nitems, size_t size);

sizedptr dir_list(const char *path);

int print(const char *fmt, ...);
char *read_full_file(const char *path);

bool write_full_file(const char *path, void* buf, size_t size);

int system(const char *command);

static inline void yield(){
    msleep(0);
}

#ifdef __cplusplus
}
#endif

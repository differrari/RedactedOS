#pragma once

#include "types.h"
#include "ui/graphic_types.h"
#include "keyboard_input.h"
#include "mouse_input.h"
#include "std/string.h"
#include "net/network_types.h"
#include "ui/draw/draw.h"
#include "files/fs.h"

#include "net/transport_layer/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void printl(const char *str);

extern void* malloc(size_t size);
extern void free(void *ptr, size_t size);

extern bool read_key(keypress *kp);
extern bool read_event(kbd_event *event);
extern void get_mouse_status(mouse_input *in);

extern void sleep(uint64_t time);
extern void halt(uint32_t exit_code);
extern uint16_t exec(const char* prog_name, int argc, const char* argv[]);

extern void request_draw_ctx(draw_ctx*);
extern void commit_draw_ctx(draw_ctx*);
extern void resize_draw_ctx(draw_ctx*, uint32_t width, uint32_t height);

extern uint32_t gpu_char_size(uint32_t scale);

extern uint64_t get_time();

extern bool socket_create(ip_version_t ipv, protocol_t protocol, uint16_t port, SockBindSpec *spec);
extern bool socket_bind(SockBindSpec *spec);
extern bool socket_listen(SockBindSpec *spec);
extern bool socket_accept(SockBindSpec *spec);
extern bool socket_send(SockBindSpec *spec, sizedptr packet);
extern bool socket_receive(SockBindSpec *spec, sizedptr *packet);
extern bool socket_close(SockBindSpec *spec);

void printf(const char *fmt, ...);

extern FS_RESULT fopen(const char* path, file* descriptor);
extern size_t fread(file *descriptor, char* buf, size_t size);
extern size_t fwrite(file *descriptor, const char* buf, size_t size);
extern void fclose(file *descriptor);
void seek(file *descriptor, int64_t offset, SEEK_TYPE type);
uintptr_t realloc(uintptr_t old_ptr, size_t old_size, size_t new_size);

sizedptr dir_list(const char *path);

#ifdef __cplusplus
}
#endif
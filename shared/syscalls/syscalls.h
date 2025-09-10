#pragma once

#include "types.h"
#include "ui/graphic_types.h"
#include "keyboard_input.h"
#include "mouse_input.h"
#include "std/string.h"
#include "net/network_types.h"
#include "ui/draw/draw.h"
#include "files/fs.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void printl(const char *str);

extern uintptr_t malloc(size_t size);
extern void free(void *ptr, size_t size);

extern bool read_key(keypress *kp);
extern void get_mouse_status(mouse_input *in);

extern void sleep(uint64_t time);
extern void halt(uint32_t exit_code);

extern void request_draw_ctx(draw_ctx*);
extern void commit_draw_ctx(draw_ctx*);

extern uint32_t gpu_char_size(uint32_t scale);

extern uint64_t get_time();

extern bool network_bind_port_current(uint16_t port);
extern bool network_unbind_port_current(uint16_t port);
extern int network_alloc_ephemeral_port_current();
extern int net_tx_frame(uintptr_t frame_ptr, uint32_t frame_len);
extern int net_rx_frame(sizedptr *out_frame);
extern bool dispatch_enqueue_frame(const sizedptr *frame);

void printf(const char *fmt, ...);

FS_RESULT fopen(const char* path, file* descriptor);
size_t fread(file *descriptor, char* buf, size_t size);
size_t fwrite(file *descriptor, const char* buf, size_t size);

sizedptr dir_list(const char *path);

#ifdef __cplusplus
}
#endif
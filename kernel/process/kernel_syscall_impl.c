#include "types.h"
#include "memory/page_allocator.h"
#include "scheduler.h"
#include "console/kio.h"
#include "input/input_dispatch.h"
#include "bin/bin_mod.h"
#include "graph/graphics.h"
#include "graph/tres.h"
#include "exceptions/timer.h"
#include "networking/transport_layer/csocket.h"
#include "net/socket_types.h"
#include "net/network_types.h"
#include "filesystem/filesystem.h"
#include "sysregs.h"
#include "memory/mmu.h"

void* malloc(size_t size){
    process_t* k = get_proc_by_pid(1);
    uintptr_t heap_pa = mmu_translate(k->heap);
    if (!heap_pa) return 0;
    return kalloc((void*)heap_pa, size, ALIGN_16B, MEM_PRIV_KERNEL);
}

void free_sized(void*ptr, size_t size){
    kfree(ptr, size);
}

extern void printl(const char *str){
    kprint(str);
}

extern bool read_key(keypress *kp){
    return sys_read_input_current(kp);
}

extern bool read_event(kbd_event *event){
    return sys_read_event_current(event);
}

extern void get_mouse_status(mouse_data *in){
    in->raw = get_raw_mouse_in();
    in->position = convert_mouse_position(get_mouse_pos());
}

extern uint16_t exec(const char* prog_name, int argc, const char* argv[]){
    process_t *p = execute(prog_name, argc, argv);
    return p->id;
}

extern void request_draw_ctx(draw_ctx* d_ctx){
    get_window_ctx(d_ctx);
}

extern void commit_draw_ctx(draw_ctx* d_ctx){
    commit_frame(d_ctx, 0);
}

extern void resize_draw_ctx(draw_ctx* d_ctx, uint32_t width, uint32_t height){
    resize_window(width, height);
    get_window_ctx(d_ctx);
    gpu_flush();
}


extern uint32_t gpu_char_size(uint32_t scale){
    return gpu_get_char_size(scale);
}

extern uint64_t get_time(){
    return timer_now_msec();
}

extern bool socket_create(Socket_Role role, protocol_t protocol, const SocketExtraOptions* extra, SocketHandle *out_handle){
    return create_socket(role, protocol, extra, get_current_proc_pid(), out_handle);
}

extern int32_t socket_bind(SocketHandle *handle, ip_version_t ip_version, uint16_t port){
    return bind_socket(handle, port, ip_version, get_current_proc_pid());
}

extern int32_t socket_connect(SocketHandle *handle, SockDstKind dst_kind, void* dst, uint16_t port){
    return connect_socket(handle, dst_kind, dst, port, get_current_proc_pid());
}

extern int32_t socket_listen(SocketHandle *handle){
    return listen_on(handle, 0, get_current_proc_pid());
}

extern bool socket_accept(SocketHandle *spec){
    accept_on_socket(spec, get_current_proc_pid());
    return 1;
}

extern size_t socket_send(SocketHandle *handle, SockDstKind dst_kind, const void* dst, uint16_t port, void *packet, size_t size){
    return send_on_socket(handle, dst_kind, dst, port, packet, size, get_current_proc_pid());
}

extern bool socket_receive(SocketHandle *handle, void *packet, size_t size, net_l4_endpoint* out_src){
    return receive_from_socket(handle, packet, size, out_src, get_current_proc_pid());
}

extern int32_t socket_close(SocketHandle *handle){
    return close_socket(handle, get_current_proc_pid());
}

extern FS_RESULT openf(const char* path, file* descriptor){
    return open_file(path, descriptor);
}

extern size_t readf(file *descriptor, char* buf, size_t size){
    return read_file(descriptor, buf, size);
}

extern size_t writef(file *descriptor, const char* buf, size_t size){
    return write_file(descriptor, buf, size);
}

extern void closef(file *descriptor){
    close_file(descriptor);
}

sizedptr dir_list(const char *path){
    kprintf("[SYSCALL implementation error] directory listing not implemented yet");
    return (sizedptr){0,0};
}

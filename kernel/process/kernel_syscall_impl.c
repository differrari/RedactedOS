#include "types.h"
#include "memory/page_allocator.h"
#include "scheduler.h"
#include "console/kio.h"
#include "input/input_dispatch.h"
#include "tools/tools.h"
#include "graph/graphics.h"
#include "graph/tres.h"
#include "exceptions/timer.h"
#include "networking/transport_layer/csocket.h"
#include "net/socket_types.h"
#include "net/network_types.h"
#include "filesystem/filesystem.h"
#include "sysregs.h"
#include "memory/mmu.h"
#include "memory/addr.h"
extern page_index *p_index;

void* malloc(size_t size){
    process_t* k = get_kernel_proc();
    if (!k) return 0;

    if (!k->heap_phys) return 0;

    void* ptr = kalloc((void*)dmap_pa_to_kva(k->heap_phys), size, ALIGN_16B, MEM_PRIV_KERNEL);
    if (ptr && size >= PAGE_SIZE && k->alloc_map)
        register_allocation(k->alloc_map, ptr, size);
    return ptr;
}

void free_sized(void*ptr, size_t size){
    kfree(ptr, size);
}

void* page_alloc(size_t size){
    if (!size) return 0;
    process_t* k = get_kernel_proc();//TODO: can we make this more fragmented? This inside a syscall, current proc outside
    void *ptr = palloc(size, MEM_PRIV_KERNEL, MEM_RW | MEM_NORM, true);
    if (k && k->alloc_map && ptr) register_allocation(k->alloc_map, ptr, size);
    return ptr;
}

void page_free(void *ptr){
    if (!ptr) return;
    if (((uintptr_t)ptr & (PAGE_SIZE - 1)) != 0) return;
    process_t* k = get_kernel_proc();//TODO: can we make this more fragmented? This inside a syscall, current proc outside

    if (k && k->alloc_map && get_alloc_size(k->alloc_map, ptr)) {
        free_registered(k->alloc_map, ptr);
        return;
    }

    if (p_index && get_alloc_size(p_index, ptr)) {
        free_registered(p_index, ptr);
        return;
    }

    pfree(ptr, PAGE_SIZE);
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
    in->raw.scroll = sys_read_scroll_current();
    in->position = convert_mouse_position(get_mouse_pos());
}

extern int32_t exec(const char* prog_name, int argc, const char* argv[], uint32_t mode){
    process_t *p = execute(prog_name, argc, argv, mode);
    return p ? (int32_t)p->id : 0;
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

extern bool statf(const char *path, fs_stat *out_stat){
    return get_stat(path, out_stat);
}

extern bool truncatef(file *descriptor, size_t size){
    return truncate(descriptor,size);
}

extern void closef(file *descriptor){
    close_file(descriptor);
}

size_t dir_list(const char *path, void *buf, size_t size, u64 *offset){
    return list_directory_contents(path, buf, size, offset);
}

bool stat(const char *path, fs_stat *out_stat){
    return get_stat(path, out_stat);
}

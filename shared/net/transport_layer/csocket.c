#include "csocket.h"
#include "csocket_tcp.h"
#include "csocket_udp.h"
#include "console/kio.h"
#include "memory/page_allocator.h"

uint16_t socket_ids;

void *sock_mem_page;

static inline void check_mem(){
    if (!sock_mem_page)
        palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
}

bool create_socket(Socket_Role role, protocol_t protocol, uint16_t pid, SocketHandle *out_handle){
    check_mem();
    socket_handle_t *in_handle;
    switch (protocol) {
        case PROTO_UDP:
            in_handle = udp_socket_create(role, pid);
        break;
        case PROTO_TCP:
            in_handle = socket_tcp_create(role, pid);
        break;
    }
    if (!in_handle){
        kprintf("[SOCKET] failed to create socket for %i",pid);
        return false;
    }
    //Hold a reference to the pointer, since that's the internal handle, and fill out_handle
    *out_handle = (SocketHandle){
        .id = socket_ids++,
        .connection = {},
        .protocol = protocol
    };
    return true;
}

int32_t bind_socket(SocketHandle *handle, uint16_t port){
    check_mem();
    socket_handle_t *sh = 0;
    protocol_t protocol = 0;//TODO: From handle
    SockBindSpec *spec = kalloc(sock_mem_page, sizeof(SockBindSpec), ALIGN_64B, MEM_PRIV_KERNEL);
    switch (protocol) {
        case PROTO_TCP:
            return socket_bind_tcp_ex(sh, spec, port);
        break;
        case PROTO_UDP:
            return socket_bind_udp_ex(sh, spec, port);
        break;
    }
    return -1;
}

int32_t connect_socket(SocketHandle *handle, uint8_t dst_kind, const void* dst, uint16_t port){
    check_mem();
    socket_handle_t *sh = 0;
    protocol_t protocol = 0;//TODO: From handle
    switch (protocol) {
        case PROTO_TCP:
            return socket_connect_tcp_ex(sh, dst_kind, dst, port);
        break;
        case PROTO_UDP:
            kprintf("[SOCKET] connect is a TCP-only function and isn't needed in UDP sockets");
        return -1;
    }
    return -1;
}

int64_t send_on_socket(SocketHandle *handle, uint8_t dst_kind, const void* dst, uint16_t port, void* buf, uint64_t len){
    check_mem();
    socket_handle_t *sh = 0;
    protocol_t protocol = 0;//TODO: From handle
    switch (protocol) {
        case PROTO_TCP:
        return socket_send_tcp(sh, buf, len);
        break;
        case PROTO_UDP:
        return socket_sendto_udp_ex(sh, dst_kind, dst, port, buf, len);
        break;
    }
    return 0;
}

int64_t receive_from_socket(SocketHandle *handle, void* buf, uint64_t len, net_l4_endpoint* out_src){
    check_mem();
    socket_handle_t *sh = 0;
    protocol_t protocol = 0;//TODO: From handle
    switch (protocol) {
        case PROTO_TCP:
        return socket_recv_tcp(sh, buf, len);
        break;
        case PROTO_UDP:
        return socket_recvfrom_udp_ex(sh, buf, len, out_src);
        break;
    }
    return 0;
}

int32_t close_socket(SocketHandle *handle){
    check_mem();
    socket_handle_t *sh = 0;
    protocol_t protocol = 0;//TODO: From handle
    switch (protocol) {
        case PROTO_TCP:
        return socket_close_tcp(sh);
        break;
        case PROTO_UDP:
        return socket_close_udp(sh);
        break;
    }
    return 0;
}

int32_t listen_on(SocketHandle *handle, int32_t backlog){
    check_mem();
    socket_handle_t *sh = 0;
    protocol_t protocol = 0;//TODO: From handle
    switch (protocol) {
        case PROTO_TCP:
            return socket_listen_tcp(sh, backlog);
        break;
        case PROTO_UDP:
            kprintf("[SOCKET] listen is a TCP-only function and isn't needed in UDP sockets");
        break;
    }
    return 0;
}

void accept_on_socket(SocketHandle *handle){
    check_mem();
    socket_handle_t *sh = 0;
    protocol_t protocol = 0;//TODO: From handle
    switch (protocol) {
        case PROTO_TCP:
            socket_accept_tcp(sh);
        break;
        case PROTO_UDP:
            kprintf("[SOCKET] accept is a TCP-only function and isn't needed in UDP sockets");
        break;
    }
}

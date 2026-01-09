#include "csocket.h"
#include "csocket_tcp.h"
#include "csocket_udp.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "data_struct/hashmap.h"

uint16_t socket_ids;

void *sock_mem_page;

chashmap_t *map;

typedef struct ksock_handle_t {
    uint16_t id;
    net_l4_endpoint connection;
    protocol_t protocol;
    void* sh;
    uint16_t pid;
} ksock_handle_t;

void* custom_alloc(size_t size){
    return kalloc(sock_mem_page, size, ALIGN_64B, MEM_PRIV_KERNEL);
}

static inline void check_mem(){
    if (!sock_mem_page)
        sock_mem_page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    if (!map){
        map = chashmap_create_alloc(128, custom_alloc, kfree);
    }
}

bool create_socket(Socket_Role role, protocol_t protocol, const SocketExtraOptions* extra, uint16_t pid, SocketHandle *out_handle){
    check_mem();
    socket_handle_t *in_handle = {};
    switch (protocol) {
        case PROTO_UDP:
        in_handle = udp_socket_create(role, pid, extra);
        break;
        case PROTO_TCP:
        in_handle = socket_tcp_create(role, pid, extra);
        break;
    }
    if (!in_handle){
        kprintf("[SOCKET] failed to create socket for %i",pid);
        return false;
    }
    out_handle->id = socket_ids;
    out_handle->connection = (net_l4_endpoint){};
    out_handle->protocol = protocol;

    kprintf("Allocating");
    ksock_handle_t *sh = (ksock_handle_t*)custom_alloc(sizeof(ksock_handle_t));
    sh->sh = in_handle;
    sh->id = out_handle->id;
    sh->protocol = protocol;
    sh->pid = pid;

    if (chashmap_put(map, &out_handle->id, sizeof(uint16_t), sh) < 0){
        kprint("Failed to save socket");
        return false;
    }
    return true;
}

int32_t bind_socket(SocketHandle *handle, uint16_t port, ip_version_t ip_version, uint16_t pid){
    check_mem();
    ksock_handle_t *sh = (ksock_handle_t*)chashmap_get(map, &handle->id, sizeof(uint16_t));
    if (sh->pid != pid){
        kprintf("[SOCKET, error] illegal socket binding");
        return 0;
    }
    protocol_t protocol = sh->protocol;
    SockBindSpec *spec = kalloc(sock_mem_page, sizeof(SockBindSpec), ALIGN_64B, MEM_PRIV_KERNEL);
    spec->kind = BIND_IP;
    spec->ver = ip_version;
    memset(spec->ip, 0, sizeof(spec->ip));
    if (ip_version == IP_VER4) memcpy(spec->ip, handle->connection.ip, 4);
    else if (ip_version == IP_VER6) memcpy(spec->ip, handle->connection.ip, 16);
    int32_t res = -1;
    switch (protocol) {
        case PROTO_TCP:
            res = socket_bind_tcp_ex(sh->sh, spec, port);
        break;
        case PROTO_UDP:
            res = socket_bind_udp_ex(sh->sh, spec, port);
        break;
    }
    return res;
}

int32_t connect_socket(SocketHandle *handle, uint8_t dst_kind, const void* dst, uint16_t port, uint16_t pid){
    check_mem();
    ksock_handle_t *sh = (ksock_handle_t*)chashmap_get(map, &handle->id, sizeof(uint16_t));
    if (sh->pid != pid){
        kprintf("[SOCKET, error] illegal socket connection");
        return 0;
    }
    protocol_t protocol = sh->protocol;
    switch (protocol) {
        case PROTO_TCP:
            return socket_connect_tcp_ex(sh->sh, dst_kind, dst, port);
        break;
        case PROTO_UDP:
            kprintf("[SOCKET] connect is a TCP-only function and isn't needed in UDP sockets");
        return -1;
    }
    return -1;
}

int64_t send_on_socket(SocketHandle *handle, uint8_t dst_kind, const void* dst, uint16_t port, void* buf, uint64_t len, uint16_t pid){
    check_mem();
    ksock_handle_t *sh = (ksock_handle_t*)chashmap_get(map, &handle->id, sizeof(uint16_t));
    if (sh->pid != pid){
        kprintf("[SOCKET, error] illegal socket send");
        return 0;
    }
    protocol_t protocol = sh->protocol;
    switch (protocol) {
        case PROTO_TCP:
        return socket_send_tcp(sh->sh, buf, len);
        break;
        case PROTO_UDP:
        return socket_sendto_udp_ex(sh->sh, dst_kind, dst, port, buf, len);
        break;
    }
    return 0;
}

int64_t receive_from_socket(SocketHandle *handle, void* buf, uint64_t len, net_l4_endpoint* out_src, uint16_t pid){
    check_mem();
    ksock_handle_t *sh = (ksock_handle_t*)chashmap_get(map, &handle->id, sizeof(uint16_t));
    if (sh->pid != pid){
        kprintf("[SOCKET, error] illegal socket receive %i, %i",sh->id,pid);
        return 0;
    }
    protocol_t protocol = sh->protocol;
    switch (protocol) {
        case PROTO_TCP:
        return socket_recv_tcp(sh->sh, buf, len);
        break;
        case PROTO_UDP:
        return socket_recvfrom_udp_ex(sh->sh, buf, len, out_src);
        break;
    }
    return 0;
}

int32_t close_socket(SocketHandle *handle, uint16_t pid){
    check_mem();
    ksock_handle_t *sh = (ksock_handle_t*)chashmap_get(map, &handle->id, sizeof(uint16_t));
    if (sh->pid != pid){
        kprintf("[SOCKET, error] illegal socket close");
        return 0;
    }
    protocol_t protocol = sh->protocol;
    switch (protocol) {
        case PROTO_TCP:
        return socket_close_tcp(sh->sh);
        break;
        case PROTO_UDP:
        return socket_close_udp(sh->sh);
        break;
    }
    return 0;
}

int32_t listen_on(SocketHandle *handle, int32_t backlog, uint16_t pid){
    check_mem();
    ksock_handle_t *sh = (ksock_handle_t*)chashmap_get(map, &handle->id, sizeof(uint16_t));
    if (sh->pid != pid){
        kprintf("[SOCKET, error] illegal socket listen");
        return 0;
    }
    protocol_t protocol = sh->protocol;
    switch (protocol) {
        case PROTO_TCP:
            return socket_listen_tcp(sh->sh, backlog);
        break;
        case PROTO_UDP:
            kprintf("[SOCKET] listen is a TCP-only function and isn't needed in UDP sockets");
        break;
    }
    return 0;
}

void accept_on_socket(SocketHandle *handle, uint16_t pid){
    check_mem();
    ksock_handle_t *sh = (ksock_handle_t*)chashmap_get(map, &handle->id, sizeof(uint16_t));
    if (sh->pid != pid){
        kprintf("[SOCKET, error] illegal socket accept");
        return;
    }
    protocol_t protocol = sh->protocol;
    switch (protocol) {
        case PROTO_TCP:
            socket_accept_tcp(sh->sh);
        break;
        case PROTO_UDP:
            kprintf("[SOCKET] accept is a TCP-only function and isn't needed in UDP sockets");
        break;
    }
}
#include "csocket.h"
#include "csocket_tcp.h"
#include "csocket_udp.h"
#include "console/kio.h"

uint16_t socket_ids;

bool create_socket(Socket_Role role, protocol_t protocol, uint16_t pid, SocketHandle *out_handle){
    void *ptr;
    switch (protocol) {
        case PROTO_UDP:
            ptr = udp_socket_create(role, pid);
        break;
        case PROTO_TCP:
            ptr = socket_tcp_create(role, pid);
        break;
    }
    if (!ptr){
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
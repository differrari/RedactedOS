#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/bmp.h"
#include "net/net.h"

#define BORDER 20

int net_example() {
    SocketHandle spec = {};
    socket_create(SOCKET_SERVER, PROTO_UDP, &spec);
    printf("Created socket for type %i",spec.protocol);
    spec.connection.ip[0] = 192;
    spec.connection.ip[1] = 168;
    spec.connection.ip[2] = 1;
    spec.connection.ip[3] = 108;
    if (socket_bind(&spec, IP_VER4, 9000) < 0) return -1;

    // socket_listen(&spec);

    void *ptr = malloc(0x1000);
    printf("Waiting for data %i.%i.%i.%i", spec.connection.ip[0],spec.connection.ip[1],spec.connection.ip[2],spec.connection.ip[3]);
    net_l4_endpoint rc = {};
    while (!socket_receive(&spec, ptr, 0x1000, &rc)){
    }

    printf("Received data from %i.%i.%i.%i:%i", rc.ip[0],rc.ip[1],rc.ip[2],rc.ip[3],rc.port);

    printf(ptr);

    // socket_accept(&spec);

    char *str = "Hello node";

    printf("Sent %i",socket_send(&spec, DST_ENDPOINT, &rc.ip, rc.port, str, strlen(str, 0)));

    socket_close(&spec);

    return 1;
}

int main(){
    net_example();
    return 0;
}
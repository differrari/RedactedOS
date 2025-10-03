#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/bmp.h"

#define BORDER 20

int proc_func() {
    SocketHandle spec = {};
    socket_create(SOCKET_CLIENT, PROTO_UDP, &spec);
    socket_bind(&spec, 9000);
    socket_listen(&spec);

    void *ptr = malloc(0x1000);
    while (!socket_receive(&spec, ptr, 0x1000, 0)){
        printf("Received data");
    }

    socket_accept(&spec);

    socket_send(&spec, 0, 0);

    socket_close(&spec);

    return 1;
}
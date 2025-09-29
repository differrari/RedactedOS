#include "default_process.h"
#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "std/string.h"
#include "image/bmp.h"

#define BORDER 20

int proc_func() {
    SockBindSpec spec = {};
    socket_create(IP_VER4, PROTO_UDP, 80, &spec);
    socket_bind(&spec);
    socket_listen(&spec);

    while (!socket_receive(&spec)){}

    socket_accept(&spec);

    socket_send(&spec);

    socket_close(&spec);

    return 1;
}
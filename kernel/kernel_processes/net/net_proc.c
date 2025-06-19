#include "net_proc.h"
#include "../kprocess_loader.h"
#include "net/network_types.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "net/udp.h"
#include "syscalls/syscalls.h"

void test_network(){
    bind_port(8888);
    network_connection_ctx dest = (network_connection_ctx){
        .ip = (192 << 24) | (168 << 16) | (1 << 8) | 255,
        .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        .port = 8080,
    };

    size_t payload_size = 5;
    char hw[5] = {'h','e','l','l','o'};

    send_packet(UDP, 8888, &dest, hw, payload_size);

    sizedptr pack;

    while (!read_packet(&pack));

    kprintf("DONE");

    sizedptr payload = udp_parse_packet_payload(pack.ptr);

    uint8_t *content = (uint8_t*)payload.ptr;

    kprintf("PAYLOAD: %s",(uintptr_t)string_ca_max(content, payload.size).data);

    stop_current_process();
    unbind_port(8888);
}

process_t* launch_net_process(){
    return create_kernel_process("nettest",test_network);
}
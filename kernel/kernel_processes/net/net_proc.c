#include "net_proc.h"
#include "../kprocess_loader.h"
#include "networking/network.h"
#include "network_types.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "networking/udp.h"

void test_network(){
    network_bind_port_current(8888);
    network_connection_ctx dest = (network_connection_ctx){
        .ip = (192 << 24) | (168 << 16) | (1 << 8) | 255,
        .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        .port = 8080,
    };

    size_t payload_size = 5;
    char hw[5] = {'h','e','l','l','o'};

    network_send_packet(UDP, 8888, &dest, hw, payload_size);

    ReceivedPacket pack;

    while (!network_read_packet_current(&pack));

    ReceivedPacket payload = udp_parse_packet_payload(pack.packet_ptr);

    uint8_t *content = (uint8_t*)payload.packet_ptr;

    kputf_raw("PAYLOAD: ");
    for (size_t i = 0; i < payload.packet_size; i++){
        kputf_raw("%c",content[i]);
    }
    kputf_raw("\n");

    stop_current_process();
    network_unbind_port_current(8888);
}

process_t* launch_net_process(){
    return create_kernel_process("nettest",test_network);
}
#include "net_proc.h"
#include "../kprocess_loader.h"
#include "networking/network.h"
#include "process/scheduler.h"

void test_network(){
    network_connection_ctx dest = (network_connection_ctx){
        .ip = (192 << 24) | (168 << 16) | (1 << 8) | 255,
        .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        .port = 8080,
    };

    size_t payload_size = 5;
    char hw[5] = {'h','e','l','l','o'};

    network_send_packet(UDP, 8888, &dest, hw, payload_size);

    stop_current_process();
}

process_t* launch_net_process(){
    return create_kernel_process("nettest",test_network);
}
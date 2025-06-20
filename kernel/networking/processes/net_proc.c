#include "net_proc.h"
#include "kernel_processes/kprocess_loader.h"
#include "net/network_types.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "net/udp.h"
#include "../protocols/dhcp.h"
#include "networking/network.h"
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

    sizedptr payload = udp_parse_packet_payload(pack.ptr);

    uint8_t *content = (uint8_t*)payload.ptr;

    kprintf("PAYLOAD: %s",(uintptr_t)string_ca_max(content, payload.size).data);

    unbind_port(8888);
}

void negotiate_dhcp(){
    network_connection_ctx *ctx = network_get_context();
    bind_port(68);
    if (ctx->ip == 0){
        send_packet(DHCP, 53, ctx, ctx->mac, 6);
    }

    sizedptr ptr;

    while (!read_packet(&ptr));

    kprintf("RECEIVED DHCP RESPONSE");

    dhcp_packet *payload = dhcp_parse_packet_payload(ptr.ptr);

    if (payload){
        uint32_t local_ip = __builtin_bswap32(payload->yiaddr);
        kprintf("Received local IP %i.%i.%i.%i",(local_ip >> 24) & 0xFF,(local_ip >> 16) & 0xFF,(local_ip >> 8) & 0xFF,(local_ip >> 0) & 0xFF);

        //We can parse options for
        //Lease time
        //DHCP Server identifier
        //Subnet mask
        //Router
        //DNS (8 bytes)
        ctx->ip = local_ip;
    }

    unbind_port(68);
    test_network();
    stop_current_process();
}

process_t* launch_net_process(){
    return create_kernel_process("nettest",negotiate_dhcp);
}
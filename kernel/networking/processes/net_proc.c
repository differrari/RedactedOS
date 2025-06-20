#include "net_proc.h"
#include "kernel_processes/kprocess_loader.h"
#include "net/network_types.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "net/udp.h"
#include "../protocols/dhcp.h"
#include "../protocols/dhcp.h"
#include "std/memfunctions.h"
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
    kprintf("Sending DHCP request");
    network_connection_ctx *ctx = network_get_context();
    bind_port(68);
    if (ctx->ip != 0)
        return;
    dhcp_request request = (dhcp_request){
        .mac = 0,
        .offered_ip = 0,
        .server_ip = 0,
    };
    memcpy(request.mac, ctx->mac, 6);
    send_packet(DHCP, 53, ctx, &request, sizeof(dhcp_request));

    sizedptr ptr;
    
    dhcp_packet *payload;

    for (int i = 5; i >= 0; i--){
        while (!read_packet(&ptr));
        kprintf("Received DHCP response");
        payload = dhcp_parse_packet_payload(ptr.ptr);
        uint16_t opt_index = dhcp_parse_option(payload, 53);
        if (payload->options[opt_index + 2] == 2)
            break;
        if (i == 0){
            sleep(60000);
            negotiate_dhcp();
            return;
        }
    }

    uint32_t local_ip = __builtin_bswap32(payload->yiaddr);
    kprintf("Received local IP %i.%i.%i.%i",(local_ip >> 24) & 0xFF,(local_ip >> 16) & 0xFF,(local_ip >> 8) & 0xFF,(local_ip >> 0) & 0xFF);
    request.offered_ip = payload->yiaddr;

    uint16_t serv_index = dhcp_parse_option(payload, 54);
    memcpy((void*)&request.server_ip, (void*)(payload->options + serv_index + 2), payload->options[serv_index+1]);

    send_packet(DHCP, 53, ctx, &request, sizeof(dhcp_request));

    for (int i = 5; i >= 0; i--){
        while (!read_packet(&ptr));
        kprintf("Received DHCP response");
        payload = dhcp_parse_packet_payload(ptr.ptr);
        uint16_t opt_index = dhcp_parse_option(payload, 53);
        if (payload->options[opt_index + 2] == 5)
            break;
        if (i == 0){
            sleep(60000);
            negotiate_dhcp();
            return;
        }
    }

    kprintf("DHCP negotiation finished");

    //We can parse options for
    //Lease time (51)
    //DHCP Server identifier
    //Subnet mask
    //Router (3)
    //DNS (8 bytes) (6)
    ctx->ip = local_ip;

    unbind_port(68);
    test_network();
    stop_current_process();
}

process_t* launch_net_process(){
    return create_kernel_process("nettest",negotiate_dhcp);
}
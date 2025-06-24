#include "net_proc.h"
#include "kernel_processes/kprocess_loader.h"
#include "net/network_types.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "net/udp.h"
#include "net/dhcp.h"
#include "std/memfunctions.h"
#include "networking/network.h"
#include "syscalls/syscalls.h"
#include "math/math.h"
#include "net/tcp.h"
#include "net/http.h"
#include "net/ipv4.h"
#include "net/eth.h"

network_connection_ctx server;

void find_server(){
    bind_port(7777);
    server = (network_connection_ctx){
        .ip = (192 << 24) | (168 << 16) | (1 << 8) | 255,
        .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        .port = 8080,
    };

    size_t payload_size = 5;
    char hw[5] = {'h','e','l','l','o'};

    send_packet(UDP, 7777, &server, hw, payload_size);

    sizedptr pack;

    while (!read_packet(&pack));

    memcpy((void*)&server.mac, (void*)eth_get_source(pack.ptr), 6);

    server.ip = ipv4_get_source(pack.ptr + sizeof(eth_hdr_t));

    sizedptr payload = udp_parse_packet_payload(pack.ptr);

    uint8_t *content = (uint8_t*)payload.ptr;

    kprintf("PAYLOAD: %s",(uintptr_t)string_ca_max(content, payload.size).data);

    unbind_port(7777);
}

void test_network(){
    find_server();
    bind_port(8888);

    sizedptr http = request_http_data(GET, &server, 8888);

    kprintf("Received payload? %x",(uintptr_t)&http);

    if (http.ptr != 0){
        kprintf("Parsing payload");
        sizedptr payload = http_get_payload(http);
        string content = http_get_chunked_payload(payload);
        printf("Received payload? %s",(uintptr_t)content.data);
    }

    unbind_port(8888);
}

uint32_t negotiate_dhcp(){
    kprintf("Sending DHCP request");
    network_connection_ctx *ctx = network_get_context();
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
        if (i == 0)
            return 60000;
    }

    uint32_t local_ip = __builtin_bswap32(payload->yiaddr);
    kprintf("Received local IP %i.%i.%i.%i",(local_ip >> 24) & 0xFF,(local_ip >> 16) & 0xFF,(local_ip >> 8) & 0xFF,(local_ip >> 0) & 0xFF);
    request.offered_ip = payload->yiaddr;

    uint16_t serv_index = dhcp_parse_option(payload, 54);
    memcpy((void*)&request.server_ip, (void*)(payload->options + serv_index + 2), min(4,payload->options[serv_index+1]));

    uint16_t lease_index = dhcp_parse_option(payload, 51);
    uint32_t lease_time;
    memcpy((void*)&lease_time, (void*)(payload->options + lease_index + 2), min(4,payload->options[lease_index+1]));

    lease_time /= 2;

    send_packet(DHCP, 53, ctx, &request, sizeof(dhcp_request));

    for (int i = 5; i >= 0; i--){
        while (!read_packet(&ptr));
        kprintf("Received DHCP response");
        payload = dhcp_parse_packet_payload(ptr.ptr);
        uint16_t opt_index = dhcp_parse_option(payload, 53);
        if (payload->options[opt_index + 2] == 5)
            break;
        if (i == 0)
            return 60000;
    }

    kprintf("DHCP negotiation finished. Lease %i",lease_time);

    //We can parse options for
    //DHCP Server identifier
    //Subnet mask
    //Router (3)
    //DNS (8 bytes) (6)
    //TODO: Make subsequent DHCP requests (renewals and requests) directed to the server
    ctx->ip = local_ip;

    test_network();

    return lease_time;
}

void dhcp_daemon(){
    bind_port(68);
    while (true){
        uint32_t await = negotiate_dhcp();
        if (await == 0) break;
        kprintf("DHCP Negotiated for %i",await);
        sleep(await);
    }
    bind_port(68);
    stop_current_process();
}

process_t* launch_net_process(){
    return create_kernel_process("dhcp_daemon",dhcp_daemon);
}
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
#include "math/math.h"
#include "../net_constants.h"
#include "net/tcp.h"

tcp_hdr_t* tcp_handskake(network_connection_ctx *dest, uint16_t port, tcp_data *data, uint8_t retry){
    if (retry == 5){
        kprintf("Exceeded max number of retries");
        return 0x0;
    } 

    send_packet(TCP, port, dest, data, sizeof(tcp_data));

    sizedptr pack;

    uint16_t timeout = 10;
    while (!read_packet(&pack)){
        sleep(1000);
        if (timeout-- == 0){
            return tcp_handskake(dest, port, data, retry+1);
        }
    }

    sizedptr payload = tcp_parse_packet_payload(pack.ptr);

    if (!payload.ptr) {
        kprintf("Wrong payload pointer. Retrying");
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    }

    tcp_hdr_t *response = (tcp_hdr_t*)payload.ptr;
    if ((response->data_offset_reserved >> 4) * 4 != payload.size){
        kprintf("Wrong payload size %i vs %i. Retrying", (response->data_offset_reserved >> 4) * 4, payload.size);
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    }

    if (response->flags != (data->flags | (1 << ACK_F))){
        kprintf("Wrong flags %b vs %b. Retrying",response->flags, data->flags | (1 << ACK_F));
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    }

    uint32_t ack = __builtin_bswap32(response->ack);
    uint32_t seq = __builtin_bswap32(response->sequence);
    if (ack != data->sequence + 1){
        kprintf("Wrong ack %i vs %i. Retrying", response->ack, data->sequence + 1);
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    }

    data->sequence = __builtin_bswap32(ack);
    data->ack = __builtin_bswap32(seq+1);
    data->flags = (1 << ACK_F);

    send_packet(TCP, port, dest, data, sizeof(tcp_data));

    kprintf("Acknowledgement of acknowledgemnt sent. Server seq = %i",seq);

    return response;
}

void test_network(){
    bind_port(8888);
    network_connection_ctx dest = (network_connection_ctx){
        .ip = HOST_IP,
        .mac = HOST_MAC,
        .port = 80,
    };

    tcp_data data = (tcp_data){
        .sequence = 0,
        .ack = 0,
        .flags = (1 << SYN_F),
        .window = UINT16_MAX,
    };

    if (!tcp_handskake(&dest, 8888, &data, 0)){
        kprintf("TCP Error");
        return;
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
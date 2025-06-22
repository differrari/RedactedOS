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
#include "../net_constants.h"
#include "net/tcp.h"
#include "net/http.h"
#include "net/ipv4.h"

void tcp_send(uint16_t port, network_connection_ctx *destination, tcp_data* data){
    send_packet(TCP, port, destination, data, sizeof(tcp_data));
    if ((data->flags & ~(1 << ACK_F)) != 0 || data->payload.size > 0){
        kprintf("The payload is %i size",data->payload.size);
        data->sequence += __builtin_bswap32(max(1,data->payload.size));
    }
    data->expected_ack = __builtin_bswap32(data->sequence);
    kprintf("Next seq %i", __builtin_bswap32(data->sequence));
}

void tcp_reset(uint16_t port, network_connection_ctx *destination, tcp_data* data){
    data->flags = (1 << RST_F) | (1 << ACK_F);
    tcp_send(port, destination, data);
}

bool tcp_expect_response(sizedptr *pack){
    uint16_t timeout = 10;
    while (!read_packet(pack)){
        sleep(1000);
        if (timeout-- == 0){
            kprintf("Response timeout");
            return false;
        }
    }
    return true;
}

uint8_t tcp_check_response(tcp_data *data, sizedptr *out){

    sizedptr pack;

    if (!tcp_expect_response(&pack) || !pack.ptr){
        kprintf("Response timeout. Retrying");
        return TCP_RETRY;
    }

    sizedptr payload = tcp_parse_packet_payload(pack.ptr);
    if (!payload.ptr) {
        kprintf("Wrong payload pointer. Retrying");
        return TCP_RETRY;
    }

    tcp_hdr_t *response = (tcp_hdr_t*)payload.ptr;

    uint32_t ack = __builtin_bswap32(response->ack);
    uint32_t seq = __builtin_bswap32(response->sequence);
    
    size_t hdr_size = (response->data_offset_reserved >> 4) * 4;
    size_t payload_size = payload.size - hdr_size;
    data->ack = __builtin_bswap32(seq+max(1,payload_size));
    
    if (ack != data->expected_ack){
        kprintf("Wrong ack %i vs %i. Resetting", ack, data->expected_ack);
        return TCP_RESET;
    } else kprintf("Received ack %i", ack);

    if (response->flags != (data->flags | (1 << ACK_F))){
        kprintf("Wrong flags %b vs %b. Resetting",response->flags, data->flags | (1 << ACK_F));
        return TCP_RESET;
    }

    if (out){
        out->ptr = payload.ptr + hdr_size;
        out->size = payload_size;
    }

    return TCP_OK;
}

bool tcp_handskake(network_connection_ctx *dest, uint16_t port, tcp_data *data, uint8_t retry){
    if (retry == 5){
        kprintf("Exceeded max number of retries");
        return false;
    } 
    
    data->sequence = 0;
    data->ack = 0;
    data->flags = (1 << SYN_F);

    tcp_send(port, dest, data);
    
    uint8_t resp = tcp_check_response(data, 0);
    if (resp == TCP_RETRY){
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    } else if (resp == TCP_RESET){
        tcp_reset(port, dest, data);
        return false;
    }

    data->flags = (1 << ACK_F);

    tcp_send(port, dest, data);

    kprintf("Acknowledgement of acknowledgemnt sent");

    return true;
}

bool tcp_close(network_connection_ctx *dest, uint16_t port, tcp_data *data, uint8_t retry, uint32_t orig_seq, uint32_t orig_ack){
    if (retry == 5){
        kprintf("Exceeded max number of retries");
        return false;
    } 

    data->sequence = orig_seq;
    data->ack = orig_ack;
    data->flags = (1 << FIN_F) | (1 << ACK_F);

    tcp_send(port, dest, data);

    data->flags = (1 << ACK_F);
    uint8_t resp = tcp_check_response(data, 0);
    if (resp == TCP_RETRY){
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    } else if (resp == TCP_RESET){
        tcp_reset(port, dest, data);
        return false;
    }

    data->flags = (1 << FIN_F);

    resp = tcp_check_response(data, 0);
    if (resp == TCP_RETRY){
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    } else if (resp == TCP_RESET){
        tcp_reset(port, dest, data);
        return true;
    }

    data->flags = (1 << ACK_F);

    tcp_send(port, dest, data);

    kprintf("Connection closed");

    return true;
}

string make_http_request(HTTPRequest request, char *domain, char *agent){
    //TODO: request instead of hardcoded GET
    return string_format("GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n\r\n",domain, agent);
}

sizedptr http_data_transfer(network_connection_ctx *dest, sizedptr payload, uint16_t port, tcp_data *data, uint8_t retry, uint32_t orig_seq, uint32_t orig_ack){
    if (retry == 5){
        kprintf("Exceeded max number of retries");
        return (sizedptr){0};
    }

    data->sequence = orig_seq;
    data->ack = orig_ack;
    data->flags = (1 << PSH_F) | (1 << ACK_F);

    data->payload = payload;

    tcp_send(port, dest, data);

    data->flags = (1 << ACK_F);

    uint8_t resp;
    do {
        resp = tcp_check_response(data, 0);
        if (resp == TCP_OK)
            break;
        if (resp == TCP_RESET)//We don't reset, we ignore irrelevant packets (or we could parse them tbh)
            continue;
        if (resp == TCP_RETRY)
            return http_data_transfer(dest, payload, port, data, retry+1, orig_seq, orig_ack);
    } while (1);

    data->flags = (1 << PSH_F) | (1 << ACK_F);

    sizedptr http_content;

    resp = tcp_check_response(data, &http_content);
    if (resp == TCP_RETRY){
        sleep(1000);
        return http_data_transfer(dest, payload, port, data, retry+1, orig_seq, orig_ack);
    } else if (resp == TCP_RESET){
        tcp_reset(port, dest, data);
        return (sizedptr){0};
    }

    data->payload = (sizedptr){0};

    data->flags = (1 << ACK_F);
    tcp_send(port, dest, data);

    return http_content;
}

sizedptr request_http_data(HTTPRequest request, network_connection_ctx *dest, uint16_t port){
    tcp_data data = (tcp_data){
        .window = UINT16_MAX,
    };

    if (!tcp_handskake(dest, 8888, &data, 0)){
        kprintf("TCP Handshake Error");
        return (sizedptr){0};
    }

    string serverstr = ipv4_to_string(dest->ip);
    string req = make_http_request(request, serverstr.data, "redactedos/0.0.1");

    free(serverstr.data, serverstr.mem_length);

    kprintf("Request");

    sizedptr http_response = http_data_transfer(dest, (sizedptr){(uintptr_t)req.data, req.length}, port, &data, 0, data.sequence, data.ack);

    kprintf("END");

    free(req.data, req.mem_length);

    if (!tcp_close(dest, 8888, &data, 0, data.sequence, data.ack)){
        kprintf("TCP Connnection not closed");
        return (sizedptr){0};
    }

    return http_response;
}

void test_network(){
    bind_port(8888);
    network_connection_ctx dest = (network_connection_ctx){
        .ip = HOST_IP,
        .mac = HOST_MAC,
        .port = 80,
    };

    sizedptr http = request_http_data(GET, &dest, 8888);

    kprintf("Received payload? %x",(uintptr_t)&http);

    if (http.ptr != 0)
        kprintf("Received payload? %s",(uintptr_t)http.ptr);

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
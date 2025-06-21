#include "tcp.h"
#include "console/kio.h"
#include "net/network_types.h"
#include "network_types.h"
#include "syscalls/syscalls.h"
#include "std/memfunctions.h"
#include "eth.h"
#include "ipv4.h"
#include "math/math.h"

void create_tcp_packet(uintptr_t p, network_connection_ctx source, network_connection_ctx destination, sizedptr payload){
    p = create_eth_packet(p, source.mac, destination.mac, 0x800);

    tcp_data *data = (tcp_data*)payload.ptr;

    size_t full_size = sizeof(tcp_hdr_t) + data->options.size + data->payload.size;

    p = create_ipv4_packet(p, full_size, 0x06, source.ip, destination.ip);

    tcp_hdr_t* tcp = (tcp_hdr_t*)p;
    tcp->src_port = __builtin_bswap16(source.port);
    tcp->dst_port = __builtin_bswap16(destination.port);

    if (payload.size != sizeof(tcp_data)){
        kprintf("[TCP Packet creation error] wrong payload size %i (expected %i)",payload.size, sizeof(tcp_data));
    }

    memcpy((void*)&tcp->sequence,(void*)payload.ptr, 12);

    p += sizeof(tcp_hdr_t);

    memcpy((void*)p,(void*)data->options.ptr, data->options.size);

    p += data->options.size;

    tcp->data_offset_reserved = ((sizeof(tcp_hdr_t) + data->options.size + 3) / 4) << 4;

    memcpy((void*)p,(void*)data->payload.ptr, data->payload.size);

    tcp->checksum = __builtin_bswap16(checksum16_pipv4(source.ip,destination.ip,0x06,(uint8_t*)tcp, full_size));
}

sizedptr tcp_parse_packet_payload(uintptr_t ptr){
    eth_hdr_t* eth = (eth_hdr_t*)ptr;
    
    ptr += sizeof(eth_hdr_t);
    
    if (__builtin_bswap16(eth->ethertype) == 0x800){
        ipv4_hdr_t* ip = (ipv4_hdr_t*)ptr;
        uint32_t srcip = __builtin_bswap32(ip->src_ip);
        ptr += sizeof(ipv4_hdr_t);
        if (ip->protocol == 0x06){
            tcp_hdr_t* tcp = (tcp_hdr_t*)ptr;
            return (sizedptr){ptr,((tcp->data_offset_reserved >> 4) & 0xF) * 4};
        }
    }

    return (sizedptr){0,0};
}

void tcp_send(uint16_t port, network_connection_ctx *destination, tcp_data* data){
    send_packet(TCP, port, destination, data, sizeof(tcp_data));
    data->expected_ack = __builtin_bswap32(data->sequence) + 1;
    if ((data->flags & ~(1 << ACK_F)) != 0)
        data->sequence += __builtin_bswap32(max(1,data->payload.size));
    // kprintf("Next seq %i", __builtin_bswap32(data->sequence));
}

void tcp_reset(uint16_t port, network_connection_ctx *destination, tcp_data* data){
    data->flags = (1 << RST_F);
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

uint8_t tcp_check_response(tcp_data *data){

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
    if (ack != data->expected_ack){
        kprintf("Wrong ack %i vs %i. Resetting", ack, data->sequence + 1);
        return TCP_RESET;
    }

    if (response->flags != (data->flags | (1 << ACK_F))){
        kprintf("Wrong flags %b vs %b. Resetting",response->flags, data->flags | (1 << ACK_F));
        return TCP_RESET;
    }

    data->ack = __builtin_bswap32(seq+max(1,(payload.size - sizeof(tcp_hdr_t) - ((response->data_offset_reserved >> 4) * 4))));

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
    
    uint8_t resp = tcp_check_response(data);
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
    uint8_t resp = tcp_check_response(data);
    if (resp == TCP_RETRY){
        sleep(1000);
        return tcp_handskake(dest, port, data, retry+1);
    } else if (resp == TCP_RESET){
        tcp_reset(port, dest, data);
        return false;
    }

    data->flags = (1 << FIN_F);

    resp = tcp_check_response(data);
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
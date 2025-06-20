#include "tcp.h"
#include "console/kio.h"
#include "net/network_types.h"
#include "network_types.h"
#include "syscalls/syscalls.h"
#include "std/memfunctions.h"
#include "eth.h"
#include "ipv4.h"

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
#include "network_dispatch.hpp"
#include "drivers/virtio_net_pci/virtio_net_pci.hpp"
#include "net/network_types.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "net/dhcp.h"
#include "net/arp.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "net/icmp.h"
#include "memory/page_allocator.h"
#include "std/memfunctions.h"
#include "hw/hw.h"
#include "network.h"

NetworkDispatch::NetworkDispatch(){
    ports = IndexMap<uint16_t>(UINT16_MAX);
    for (uint16_t i = 0; i < UINT16_MAX; i++)
        ports[i] = UINT16_MAX;
    context = (network_connection_ctx) {0};
}

NetDriver* NetworkDispatch::select_driver(){
    return BOARD_TYPE == 1 ? VirtioNetDriver::try_init() : 0x0;
}

bool NetworkDispatch::init(){
    if ((driver = select_driver())){
        driver->get_mac(&context);
        return true;
    }
    return false;
}

bool NetworkDispatch::bind_port(uint16_t port, uint16_t process){
    if (ports[port] != UINT16_MAX) return false;
    ports[port] = process;
    return true;
}

bool NetworkDispatch::unbind_port(uint16_t port, uint16_t process){
    if (ports[port] != process) return false;
    ports[port] = UINT16_MAX;
    return true;
}

void NetworkDispatch::handle_download_interrupt(){
    if (driver){
        void *buffer = kalloc((void*)get_current_heap(), MAX_PACKET_SIZE, ALIGN_16B, get_current_privilege(), false);
        sizedptr packet = driver->handle_receive_packet(buffer);
        bool need_free = true;
        uintptr_t ptr = packet.ptr;
        if (ptr){
            eth_hdr_t *eth = (eth_hdr_t*)ptr;
            uint16_t ethtype = eth_parse_packet_type(ptr);
            ptr += sizeof(eth_hdr_t);
            if (ethtype == 0x806){
                arp_hdr_t *arp = (arp_hdr_t*)ptr;
                if (arp_should_handle(arp, get_context()->ip)){
                    kprintf("Received an ARP request");
                    bool req = 0;
                    network_connection_ctx conn;
                    arp_populate_response(&conn, arp);
                    send_packet(ARP, 0, &conn, &req, 1);
                }
                //TODO: Should also look for responses to our own queries
            } else if (ethtype == 0x800){//IPV4
                ipv4_hdr_t *ipv4 = (ipv4_hdr_t*)ptr;
                uint8_t protocol = ipv4_get_protocol(ptr);
                ptr += sizeof(ipv4_hdr_t);
                if (protocol == 0x11 || protocol == 0x06){
                    uint16_t port = udp_parse_packet(ptr);
                    if (ports[port] != UINT16_MAX){
                        process_t *proc = get_proc_by_pid(ports[port]);
                        if (!proc)
                            unbind_port(port, ports[port]);
                        else {
                            packet_buffer_t* buf = &proc->packet_buffer;
                            uint32_t next_index = (buf->write_index + 1) % PACKET_BUFFER_CAPACITY;

                            buf->entries[buf->write_index] = packet;
                            buf->write_index = next_index;

                            need_free = false;

                            if (buf->write_index == buf->read_index)
                                buf->read_index = (buf->read_index + 1) % PACKET_BUFFER_CAPACITY;
                        }
                    }
                } else if (protocol == 0x1) {
                    icmp_data data = (icmp_data){
                        .response = true
                    };
                    network_connection_ctx conn;
                    icmp_packet *icmp = (icmp_packet*)ptr;
                    data.seq = icmp_get_sequence(icmp);
                    data.id = icmp_get_id(icmp);
                    icmp_copy_payload(&data.payload, icmp);
                    ipv4_populate_response(&conn, eth, ipv4);
                    send_packet(ICMP, 0, &conn, &data, sizeof(icmp_data));
                }
            }
        }
        if (need_free){
            kfree(buffer, MAX_PACKET_SIZE);
        }
    }
}

void NetworkDispatch::handle_upload_interrupt(){
    driver->handle_sent_packet();
}

bool NetworkDispatch::read_packet(sizedptr *Packet, uint16_t process){
    process_t *proc = get_proc_by_pid(process);
    if (proc->packet_buffer.read_index == proc->packet_buffer.write_index) return false;

    sizedptr original = proc->packet_buffer.entries[proc->packet_buffer.read_index];
    
    uintptr_t copy = (uintptr_t)kalloc((void*)get_current_heap(), original.size, ALIGN_16B, get_current_privilege(), false);
    memcpy((void*)copy,(void*)original.ptr,original.size);
    Packet->ptr = copy;
    Packet->size = original.size;
    free_sized(original);
    proc->packet_buffer.read_index = (proc->packet_buffer.read_index + 1) % PACKET_BUFFER_CAPACITY;
    return true;
}

void NetworkDispatch::send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len){
    sizedptr packet_buffer;
    switch (protocol) {
        case UDP:
            packet_buffer = driver->allocate_packet(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + payload_len);
            context.port = port;
            create_udp_packet(packet_buffer.ptr + driver->header_size, context, *destination, (sizedptr){(uintptr_t)payload, payload_len});
        break;
        case DHCP:
            packet_buffer = driver->allocate_packet(DHCP_SIZE);
            create_dhcp_packet(packet_buffer.ptr + driver->header_size, (dhcp_request*)payload);
            break;
        case ARP:
            packet_buffer = driver->allocate_packet(sizeof(eth_hdr_t) + sizeof(arp_hdr_t));
            create_arp_packet(packet_buffer.ptr + driver->header_size, context.mac, context.ip, destination->mac, destination->ip, *(bool*)payload);
            break;
        case ICMP:
            packet_buffer = driver->allocate_packet(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_packet));
            create_icmp_packet(packet_buffer.ptr + driver->header_size, context, *destination, (icmp_data*)payload);
            break;
        case TCP:
            tcp_data *data = (tcp_data*)payload;
            packet_buffer = driver->allocate_packet(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t) + data->options.size + data->payload.size);
            context.port = port;
            create_tcp_packet(packet_buffer.ptr + driver->header_size, context, *destination, (sizedptr){(uintptr_t)data, sizeof(tcp_data)});
            break;
    }
    if (driver)
        driver->send_packet(packet_buffer);
}

network_connection_ctx* NetworkDispatch::get_context(){
    return &context;
}
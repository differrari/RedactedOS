#include "network_dispatch.hpp"
#include "drivers/virtio_net_pci/virtio_net_pci.hpp"
#include "net/network_types.h"
#include "console/kio.h"
#include "process/scheduler.h"
#include "net/udp.h"
#include "net/eth.h"
#include "memory/page_allocator.h"
#include "std/memfunctions.h"
#include "protocols/arp.h"

NetworkDispatch::NetworkDispatch(){
    ports = IndexMap<uint16_t>(UINT16_MAX);
    for (uint16_t i = 0; i < UINT16_MAX; i++)
        ports[i] = UINT16_MAX;
}

bool NetworkDispatch::init(){
    if (VirtioNetDriver *vnd = VirtioNetDriver::try_init()){
        driver = vnd;
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

void NetworkDispatch::handle_interrupt(){
    if (driver){
        sizedptr packet = driver->handle_receive_packet();
        if (packet.ptr){
            uint16_t ethtype = eth_parse_packet_type(packet.ptr);
            if (ethtype == 0x806){
                if (arp_should_handle(packet.ptr + sizeof(eth_hdr_t), get_context()->ip))
                    kprintf("We've received an ARP packet, but it's too late to write a parser for it rn. gn");
                    //create_arp_packet((uint8_t*)(buf_ptr + sizeof(virtio_net_hdr_t)), connection_context.mac, connection_context.ip, destination->mac, destination->ip, *(bool*)payload);
            } else if (ethtype == 0x800){//IPV4
                uint16_t port = udp_parse_packet(packet.ptr + sizeof(eth_hdr_t));
                if (ports[port] != UINT16_MAX){
                    process_t *proc = get_proc_by_pid(ports[port]);
                    if (!proc)
                        unbind_port(port, ports[port]);
                    else {
                        packet_buffer_t* buf = &proc->packet_buffer;
                        uint32_t next_index = (buf->write_index + 1) % PACKET_BUFFER_CAPACITY;

                        buf->entries[buf->write_index] = packet;
                        buf->write_index = next_index;

                        if (buf->write_index == buf->read_index)
                            buf->read_index = (buf->read_index + 1) % PACKET_BUFFER_CAPACITY;
                    }
                }
            }
        }
    }
}

bool NetworkDispatch::read_packet(sizedptr *Packet, uint16_t process){
    process_t *proc = get_proc_by_pid(process);
    if (proc->packet_buffer.read_index == proc->packet_buffer.write_index) return false;

    sizedptr original = proc->packet_buffer.entries[proc->packet_buffer.read_index];
    
    uintptr_t copy = (uintptr_t)allocate_in_page((void*)get_current_heap(), original.size, ALIGN_16B, get_current_privilege(), false);
    memcpy((void*)copy,(void*)original.ptr,original.size);
    Packet->ptr = copy;
    Packet->size = original.size;
    proc->packet_buffer.read_index = (proc->packet_buffer.read_index + 1) % PACKET_BUFFER_CAPACITY;
    return true;
}

void NetworkDispatch::send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len){
    if (driver)
        driver->send_packet(protocol, port, destination, payload, payload_len);
}

network_connection_ctx* NetworkDispatch::get_context(){
    return &driver->connection_context;
}
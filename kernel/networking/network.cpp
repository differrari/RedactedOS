#include "network.h"
#include "network_dispatch.hpp"
#include "std/allocator.hpp"
#include "process/scheduler.h"

NetworkDispatch *dispatch;

bool network_init(){
    dispatch = new NetworkDispatch();
    return dispatch->init();
}

void network_handle_interrupt(){
    return dispatch->handle_interrupt();
}

bool network_bind_port(uint16_t port, uint16_t process){
    return dispatch->bind_port(port, process);
}

bool network_unbindbind_port(uint16_t port, uint16_t process){
    return dispatch->unbind_port(port, process);
}

bool network_bind_port_current(uint16_t port){
    return dispatch->bind_port(port, get_current_proc_pid());
}

bool network_unbind_port_current(uint16_t port){
    return dispatch->unbind_port(port, get_current_proc_pid());
}


void network_send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len){
    return dispatch->send_packet(protocol, port, destination, payload, payload_len);
}

bool network_read_packet(ReceivedPacket *packet, uint16_t process){
    return dispatch->read_packet(packet, process);
}

bool network_read_packet_current(ReceivedPacket *packet){
    return dispatch->read_packet(packet, get_current_proc_pid());
}

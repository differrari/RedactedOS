#include "network_dispatch.hpp"
#include "drivers/virtio_net_pci/virtio_net_pci.hpp"

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
    if (driver)
        driver->handle_interrupt();
}

void NetworkDispatch::send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len){
    if (driver)
        driver->send_packet(protocol, port, destination, payload, payload_len);
}
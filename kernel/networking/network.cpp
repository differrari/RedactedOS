#include "network.h"
#include "networking/transport_layer/socket_core.h"
#include "networking/firewall.h"
#include "network_dispatch.hpp"

static NetworkDispatch *dispatch = 0;

bool network_init(system_module *mod) {
    dispatch = new NetworkDispatch();
    if (!dispatch) return false;
    return dispatch->init();
}

void network_handle_download_interrupt_nic(uint16_t nic_id) {
}

void network_handle_upload_interrupt_nic(uint16_t nic_id) {
}

int network_net_task_entry(int argc, char* argv[]) {
    if (dispatch) return dispatch->net_task();
    return 0;
}

int net_tx_packet_on(uint8_t ifindex, netpkt_t* pkt) {
    if (!dispatch || !pkt || !netpkt_len(pkt)) return -1;
    return dispatch->enqueue_packet(ifindex, pkt) ? 0 : -1;
}

const uint8_t* network_get_mac(uint8_t ifindex) {
    return dispatch ? dispatch->mac(ifindex) : nullptr;
}

uint16_t network_get_device_mtu(uint8_t ifindex) {
    if (!dispatch) return 0;
    return dispatch->device_mtu(ifindex);
}

uint16_t network_get_header_size(uint8_t ifindex) {
    if (!dispatch) return 0;
    return dispatch->header_size(ifindex);
}

const char* network_get_ifname(uint8_t ifindex) {
    if (!dispatch) return 0;
    return dispatch->ifname(ifindex);
}

void network_dump_interfaces() {
    if (dispatch) dispatch->dump_interfaces();
}

bool network_sync_multicast(uint8_t ifindex, const uint8_t* macs, uint32_t count) {
    if (!dispatch) return false;
    NetDriver* drv = dispatch->driver_at(ifindex);
    if (!drv) return false;
    return drv->sync_multicast(macs, count);
}

void network_cleanup_process(uint16_t pid) {
    firewall_cleanup_process(pid);
    socket_core_close_process(pid);
}

system_module net_module = (system_module){
    .name = "net",
    .mount = "net",
    .version = VERSION_NUM(0, 1, 0, 1),
    .owner = 0,
    .init = network_init,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .close = 0,
    .truncate = 0,
    .getstat = 0,
    .readdir = 0,
    .alias_info = {}
};
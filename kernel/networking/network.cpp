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

int net_tx_packet_on(uint16_t ifindex, netpkt_t* pkt) {
    if (!dispatch || !pkt || !netpkt_len(pkt)) return -1;
    return dispatch->enqueue_packet((uint8_t)ifindex, pkt) ? 0 : -1;
}

const uint8_t* network_get_mac(uint16_t ifindex) {
    static uint8_t dummy[6] = {0,0,0,0,0,0};
    if (!dispatch) return dummy;
    const uint8_t* m = dispatch->mac(ifindex);
    return m ? m : dummy;
}

uint16_t network_get_mtu(uint16_t ifindex) {
    if (!dispatch) return 0;
    return dispatch->mtu(ifindex);
}

uint16_t network_get_header_size(uint16_t ifindex) {
    if (!dispatch) return 0;
    return dispatch->header_size(ifindex);
}

const char* network_get_ifname(uint16_t ifindex) {
    if (!dispatch) return 0;
    return dispatch->ifname(ifindex);
}

const char* network_get_hw_ifname(uint16_t ifindex) {
    if (!dispatch) return 0;
    return dispatch->hw_ifname(ifindex);
}

size_t network_nic_count() {
    if (!dispatch) return 0;
    return dispatch->nic_count();
}


uint16_t network_net_get_pid() {
    return dispatch ? dispatch->get_net_pid() : UINT16_MAX;
}

void network_dump_interfaces() {
    if (dispatch) dispatch->dump_interfaces();
}

bool network_sync_multicast(uint16_t ifindex, const uint8_t* macs, uint32_t count) {
    if (!dispatch) return false;
    NetDriver* drv = dispatch->driver_at((uint8_t)ifindex);
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
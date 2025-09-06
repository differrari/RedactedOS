#include "network.h"
#include "network_dispatch.hpp"
#include "process/scheduler.h"

static NetworkDispatch *dispatch = 0;

bool network_init() {
    dispatch = new NetworkDispatch();
    if (!dispatch) return false;
    return dispatch->init();
}

void network_handle_download_interrupt() {
    if (dispatch) dispatch->handle_rx_irq(0);
}

void network_handle_upload_interrupt() {
    if (dispatch) dispatch->handle_tx_irq(0);
}

void network_handle_download_interrupt_nic(uint16_t nic_id) {
    if (dispatch) dispatch->handle_rx_irq((size_t)nic_id);
}

void network_handle_upload_interrupt_nic(uint16_t nic_id) {
    if (dispatch) dispatch->handle_tx_irq((size_t)nic_id);
}

int network_net_task_entry(int argc, char* argv[]) {
    if (dispatch) return dispatch->net_task();
    return 0;
}

int net_tx_frame(uintptr_t frame_ptr, uint32_t frame_len) {
    if (!dispatch || !frame_ptr || !frame_len) return -1;
    return dispatch->enqueue_frame_on(0, {frame_ptr, frame_len}) ? 0 : -1;
}

int net_tx_frame_on(uint16_t nic_id, uintptr_t frame_ptr, uint32_t frame_len) {
    if (!dispatch || !frame_ptr || !frame_len) return -1;
    return dispatch->enqueue_frame_on((size_t)nic_id, {frame_ptr, frame_len}) ? 0 : -1;
}

int net_rx_frame(sizedptr* out_frame) {
    if (!out_frame) return -1;
    out_frame->ptr = 0;
    out_frame->size = 0;
    return 0;
}
const uint8_t* network_get_local_mac() {
    static uint8_t dummy[6] = {0,0,0,0,0,0};
    if (!dispatch) return dummy;
    return dispatch->mac(0);
}

const uint8_t* network_get_mac(uint16_t nic_id) {
    static uint8_t dummy[6] = {0,0,0,0,0,0};
    if (!dispatch) return dummy;
    const uint8_t* m = dispatch->mac((size_t)nic_id);
    return m ? m : dummy;
}

uint16_t network_get_mtu(uint16_t nic_id) {
    if (!dispatch) return 0;
    return dispatch->mtu((size_t)nic_id);
}

uint16_t network_get_header_size(uint16_t nic_id) {
    if (!dispatch) return 0;
    return dispatch->header_size((size_t)nic_id);
}

uint8_t network_get_ifindex(uint16_t nic_id) {
    if (!dispatch) return 0xFF;
    return dispatch->ifindex((size_t)nic_id);
}

const char* network_get_ifname(uint16_t nic_id) {
    if (!dispatch) return 0;
    return dispatch->ifname((size_t)nic_id);
}

const char* network_get_hw_ifname(uint16_t nic_id) {
    if (!dispatch) return 0;
    return dispatch->hw_ifname((size_t)nic_id);
}

size_t network_nic_count() {
    if (!dispatch) return 0;
    return dispatch->nic_count();
}

void network_net_set_pid(uint16_t pid) {
    if (dispatch) dispatch->set_net_pid(pid);
}

uint16_t network_net_get_pid() {
    return dispatch ? dispatch->get_net_pid() : UINT16_MAX;
}

driver_module net_module = (driver_module){
    .name = "net",
    .mount = "/net",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = network_init,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .seek = 0,
    .readdir = 0,
};
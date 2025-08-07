#include "network.h"
#include "network_dispatch.hpp"
#include "process/scheduler.h"

static NetworkDispatch *dispatch = nullptr;

extern "C" bool network_init() {
    dispatch = new NetworkDispatch();
    return dispatch && dispatch->init();
}

extern "C" void network_handle_download_interrupt() {
    if (dispatch) dispatch->handle_download_interrupt();
}

extern "C" void network_handle_upload_interrupt() {
    if (dispatch) dispatch->handle_upload_interrupt();
}

extern "C" void network_net_task_entry() {
    if (dispatch) dispatch->net_task();
}

extern "C" int net_tx_frame(uintptr_t frame_ptr, uint32_t frame_len) {
    if (!dispatch || !frame_ptr || !frame_len) return -1;
    return dispatch->enqueue_frame({frame_ptr, frame_len}) ? 0 : -1;
}

extern "C" int net_rx_frame(sizedptr *out_frame) {
    extern uint16_t get_current_proc_pid();
    if (!dispatch || !out_frame) return -1;
    int sz = dispatch->dequeue_packet_for(get_current_proc_pid(), out_frame) ? (int)out_frame->size : 0;
    return sz;
}

extern "C" const net_l2l3_endpoint* network_get_local_endpoint() {
    static net_l2l3_endpoint dummy = {0};
    return dispatch ? &dispatch->get_local_ep() : &dummy;
}

extern "C" void network_net_set_pid(uint16_t pid) {
    if (dispatch) dispatch->set_net_pid(pid);
}

extern "C" uint16_t network_net_get_pid() {
    return dispatch ? dispatch->get_net_pid() : UINT16_MAX;
}

#include "network.h"
#include "network_dispatch.hpp"
#include "std/allocator.hpp"

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

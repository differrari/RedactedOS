#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"

#define NET_IRQ 32
//TODO: consider using the system MTU here
#define MAX_PACKET_SIZE 0x1000

void network_net_set_pid(uint16_t pid);
uint16_t network_net_get_pid();

bool network_init();
void network_handle_download_interrupt();
void network_handle_upload_interrupt();
void network_net_task_entry();

int net_tx_frame(uintptr_t frame_ptr, uint32_t frame_len);
int net_rx_frame(sizedptr *out_frame);

const net_l2l3_endpoint* network_get_local_endpoint();
void network_update_local_ip(uint32_t ip);

#ifdef __cplusplus
}
#endif

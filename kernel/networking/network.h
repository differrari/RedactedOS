#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"
#include "dev/driver_base.h"

#define NET_IRQ_BASE 40
//TODO: consider using the system MTU here
#define MAX_PACKET_SIZE 0x1000

void network_net_set_pid(uint16_t pid);
uint16_t network_net_get_pid();

bool network_init();
void network_handle_download_interrupt_nic(uint16_t nic_id);
void network_handle_upload_interrupt_nic(uint16_t nic_id);
int network_net_task_entry(int argc, char* argv[]);

int net_tx_frame(uintptr_t frame_ptr, uint32_t frame_len);
int net_tx_frame_on(uint16_t ifindex, uintptr_t frame_ptr, uint32_t frame_len);
int net_rx_frame(sizedptr *out_frame);

const uint8_t* network_get_local_mac(void);
const uint8_t* network_get_mac(uint16_t ifindex);
uint16_t network_get_mtu(uint16_t ifindex);
uint16_t network_get_header_size(uint16_t ifindex);
const char* network_get_ifname(uint16_t ifindex);
const char* network_get_hw_ifname(uint16_t ifindex);
size_t network_nic_count(void);

void network_update_local_ip(uint32_t ip);

void network_dump_interfaces(void);
bool network_sync_multicast(uint16_t ifindex, const uint8_t* macs, uint32_t count);

extern system_module net_module;

#ifdef __cplusplus
}
#endif

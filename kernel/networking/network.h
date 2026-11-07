#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"
#include "networking/netpkt.h"
#include "files/system_module.h"

#define NET_IRQ_BASE 40
//TODO: consider using the system MTU here
#define MAX_PACKET_SIZE 0x1000


bool network_init();
void network_handle_download_interrupt_nic(uint16_t nic_id);
void network_handle_upload_interrupt_nic(uint16_t nic_id);
int network_net_task_entry(int argc, char* argv[]);

int net_tx_packet_on(uint8_t ifindex, netpkt_t* pkt);

const uint8_t* network_get_mac(uint8_t ifindex);
uint16_t network_get_device_mtu(uint8_t ifindex);
uint16_t network_get_header_size(uint8_t ifindex);
const char* network_get_ifname(uint8_t ifindex);

void network_cleanup_process(uint16_t pid);

void network_dump_interfaces(void);
bool network_sync_multicast(uint8_t ifindex, const uint8_t* macs, uint32_t count);

extern system_module net_module;

#ifdef __cplusplus
}
#endif

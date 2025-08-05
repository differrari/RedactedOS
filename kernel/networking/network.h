#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"

#define NET_IRQ 32

//TODO: review this number
#define MAX_PACKET_SIZE 0x1000

bool network_init();
void network_handle_download_interrupt();
void network_handle_upload_interrupt();
bool network_bind_port(uint16_t port, uint16_t process);
bool network_unbind_port(uint16_t port, uint16_t process);
void network_send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len);

bool network_bind_port_current(uint16_t port);
bool network_unbind_port_current(uint16_t port);

bool network_read_packet(sizedptr *packet, uint16_t process);
bool network_read_packet_current(sizedptr *packet);

network_connection_ctx* network_get_context();

#ifdef __cplusplus
}
#endif
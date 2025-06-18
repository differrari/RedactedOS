#include "virtio_net_pci.h"
#include "std/string.h"
#include "memory/memory_access.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "pci.h"
#include "virtio/virtio_pci.h"
#include "std/memfunctions.h"
#include "packets.h"
#include "exceptions/irq.h"
/*
net_constants.h is gitignored and needs to be created. It should contain a {0xXX,0xXX,0xXX,0xXX,0xXX,0xXX}' uint8_t[6] called HOST_MAC  and '(192 << 24) | (168 << 16) | (1 << 8) | x' uint64_t called HOST_IP.
These are used to test networking capabilities, if you want to test networking on the system, you should also set up a server on the receiving computer. It should be fine if packets sent from here are ignored on the receiving side.
Eventually, this networking functionality can be used to transfer data between the host computer (or another server) and REDACTED OS.
A separate project for the server code will be provided once that's necessary, but for now it just listens for UDP on port 8080 and optionally sends UDP packets as a response
*/
#include "net_constants.h"

static bool vnp_verbose = false;

void vnp_enable_verbose(){
    vnp_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (vnp_verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

static virtio_device vnp_net_dev;

#define RECEIVE_QUEUE 0
#define TRANSMIT_QUEUE 1

#define VIRTIO_NET_HDR_GSO_NONE 0

#define UDP_PACKET_SIZE sizeof(virtio_net_hdr_t) + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t)
#define MAX_PACKET_SIZE 0x1000

typedef struct __attribute__((packed)) virtio_net_hdr_t {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t num_buffers;
} virtio_net_hdr_t;

uint16_t udp_checksum(
    uint32_t src_ip,
    uint32_t dst_ip,
    uint8_t protocol,
    const uint8_t* udp_header_and_payload,
    uint16_t length
) {
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += protocol;
    sum += length;

    for (uint16_t i = 0; i + 1 < length; i += 2)
        sum += (udp_header_and_payload[i] << 8) | udp_header_and_payload[i + 1];

    if (length & 1)
        sum += udp_header_and_payload[length - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

void create_udp_packet(
    uint8_t* buf,
    const uint8_t src_mac[6],
    const uint8_t dst_mac[6],
    uint32_t src_ip,
    uint32_t dst_ip,
    uint16_t src_port,
    uint16_t dst_port,
    const uint8_t* payload,
    uint16_t payload_len
) {
    uintptr_t p = (uintptr_t)buf;

    virtio_net_hdr_t* net = (virtio_net_hdr_t*)p;
    
    net->flags = 0;
    net->gso_type = VIRTIO_NET_HDR_GSO_NONE;
    net->hdr_len = 0;
    net->gso_size = 0;
    net->csum_start = 0;
    net->num_buffers = 0;

    p += sizeof(virtio_net_hdr_t);

    eth_hdr_t* eth = (eth_hdr_t*)p;
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = src_mac[i];
    eth->ethertype = __builtin_bswap16(0x0800);
    p += sizeof(eth_hdr_t);

    ipv4_hdr_t* ip = (ipv4_hdr_t*)p;
    ip->version_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_length = __builtin_bswap16(sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + payload_len);
    ip->identification = 0;
    ip->flags_frag_offset = __builtin_bswap16(0x4000);
    ip->ttl = 64;
    ip->protocol = 0x11;
    ip->header_checksum = 0;
    ip->src_ip = __builtin_bswap32(src_ip);
    ip->dst_ip = __builtin_bswap32(dst_ip);
    uint16_t* ip_words = (uint16_t*)ip;
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) sum += ip_words[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->header_checksum = ~sum;
    p += sizeof(ipv4_hdr_t);

    udp_hdr_t* udp = (udp_hdr_t*)p;
    udp->src_port = __builtin_bswap16(src_port);
    udp->dst_port = __builtin_bswap16(dst_port);
    udp->length = __builtin_bswap16(sizeof(udp_hdr_t) + payload_len);

    p += sizeof(udp_hdr_t);

    uint8_t* data = (uint8_t*)p;
    for (int i = 0; i < payload_len; i++) data[i] = payload[i];

    udp->checksum = __builtin_bswap16(udp_checksum(src_ip,dst_ip,ip->protocol,(uint8_t*)udp,sizeof(udp_hdr_t) + payload_len));
    
}

typedef struct virtio_net_config { 
        uint8_t mac[6]; 
        uint16_t status; 
        uint16_t max_virtqueue_pairs; 
        uint16_t mtu; 
        uint32_t speed; 
        uint8_t duplex; 
        uint8_t rss_max_key_size; 
        uint16_t rss_max_indirection_table_length; 
        uint32_t supported_hash_types; 
}__attribute__((packed)) virtio_net_config; 

bool vnp_find_network(){
    uint64_t addr = find_pci_device(0x1AF4, 0x1000);
    if (!addr){ 
        kprintf("[VIRTIO_NET error] Virtio network device not found");
        return false;
    }
    
    uint64_t net_device_address, net_device_size;
    
    kprintfv("[VIRTIO_NET] Configuring network device");
    
    virtio_get_capabilities(&vnp_net_dev, addr, &net_device_address, &net_device_size);
    pci_register(net_device_address, net_device_size);

    uint8_t interrupts_ok = pci_setup_interrupts(addr, NET_IRQ);
    switch(interrupts_ok){
        case 0:
            kprintf_raw("[VIRTIO_NET] Failed to setup interrupts");
            return false;
        case 1:
            kprintf_raw("[VIRTIO_NET] Interrupts setup with MSI-X %i",NET_IRQ);
            break;
        default:
            kprintf_raw("[VIRTIO_NET] Interrupts setup with MSI %i",NET_IRQ);
            break;
    }

    pci_enable_device(addr);

    if (!virtio_init_device(&vnp_net_dev)) {
        kprintf("[VIRTIO_NET error] Failed network initialization");
        return false;
    }

    virtio_net_config* net_config = (virtio_net_config*)vnp_net_dev.device_cfg;
    kprintfv("[VIRTIO_NET] %x:%x:%x:%x:%x:%x", net_config->mac[0], net_config->mac[1], net_config->mac[2], net_config->mac[3], net_config->mac[4], net_config->mac[5]);

    kprintfv("[VIRTIO_NET] %i virtqueue pairs",net_config->max_virtqueue_pairs);
    kprintfv("[VIRTIO_NET] %x speed", net_config->speed);
    kprintfv("[VIRTIO_NET] status = %x", net_config->status);

    uint8_t dest[6] = HOST_MAC;

    kprintf("[VIRTIO_NET] %x:%x:%x:%x:%x:%x", dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
    
    size_t payload_size = 5;
    char hw[5] = {'h','e','l','l','o'};

    select_queue(&vnp_net_dev, RECEIVE_QUEUE);

    for (uint16_t i = 0; i < 128; i++){
        void* buf = allocate_in_page(vnp_net_dev.memory_page, MAX_PACKET_SIZE, ALIGN_64B, true, true);
        virtio_add_buffer(&vnp_net_dev, i, (uintptr_t)buf, MAX_PACKET_SIZE);
    }

    kprintfv("[VIRTIO_NET] Current MSI-X queue index %i",vnp_net_dev.common_cfg->queue_msix_vector);
    vnp_net_dev.common_cfg->queue_msix_vector = 0;
    if (vnp_net_dev.common_cfg->queue_msix_vector != 0){
        kprintfv("[VIRTIO_NET error] failed to set interrupts on receive queue, network will be unable to receive packets");
        return false;
    }
    
    void* test_packet = allocate_in_page(vnp_net_dev.memory_page, UDP_PACKET_SIZE + payload_size, ALIGN_64B, true, true);
    create_udp_packet(test_packet,net_config->mac,dest,(192 << 24) | (168 << 16) | (1 << 8) | 131,HOST_IP,8888,8080,hw, payload_size);

    kprintfv("[VIRTIO_NET] Packet created");

    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);
    kprintfv("[VIRTIO_NET] New MSI-X queue index %x",vnp_net_dev.common_cfg->msix_config);

    size_t size = UDP_PACKET_SIZE + payload_size;
    
    kprintfv("[VIRTIO_NET] Upload queue selected for packet %x (size %i)",(uintptr_t)test_packet, size);

    // virtio_send_1d(&vnp_net_dev, (uintptr_t)test_packet, size);

    kprintfv("[VIRTIO_NET] Packet sent");

    return true;
}

void vnp_handle_interrupt(){
    kprintf(">>>>>>>>Received network interrupt");
}
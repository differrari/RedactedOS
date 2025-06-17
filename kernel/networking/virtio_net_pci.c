#include "virtio_net_pci.h"
#include "std/string.h"
#include "memory/memory_access.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "pci.h"
#include "virtio/virtio_pci.h"
#include "std/memfunctions.h"
#include "packets.h"

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
    sum += __builtin_bswap16(protocol);
    sum += __builtin_bswap16(length);

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
    ip->protocol = 17;
    ip->header_checksum = 0;
    ip->src_ip = __builtin_bswap32(src_ip);
    ip->dst_ip = __builtin_bswap32(dst_ip);
    uint16_t* ip_words = (uint16_t*)ip;
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) sum += ip_words[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->header_checksum = __builtin_bswap16(~sum);
    p += sizeof(ipv4_hdr_t);

    udp_hdr_t* udp = (udp_hdr_t*)p;
    udp->src_port = __builtin_bswap16(src_port);
    udp->dst_port = __builtin_bswap16(dst_port);
    udp->length = __builtin_bswap16(sizeof(udp_hdr_t) + payload_len);
    udp->checksum = udp_checksum(ip->src_ip,ip->dst_ip,17,(uint8_t*)udp,sizeof(udp_hdr_t) + payload_len);
    p += sizeof(udp_hdr_t);

    uint8_t* data = (uint8_t*)p;
    for (int i = 0; i < payload_len; i++) data[i] = payload[i];
    
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
    virtio_enable_verbose();
    uint64_t addr = find_pci_device(0x1AF4, 0x1000);
    if (!addr){ 
        kprintf("Virtio network device not found");
        return false;
    }
    
    pci_enable_device(addr);

    uint64_t net_device_address, net_device_size;

    kprintf("Configuring network device");

    virtio_get_capabilities(&vnp_net_dev, addr, &net_device_address, &net_device_size);
    pci_register(net_device_address, net_device_size);
    if (!virtio_init_device(&vnp_net_dev)) {
        kprintf("Failed network initialization");
        return false;
    }

    virtio_net_config* net_config = (virtio_net_config*)vnp_net_dev.device_cfg;
    kprintf("%x:%x:%x:%x:%x:%x", net_config->mac[0], net_config->mac[1], net_config->mac[2], net_config->mac[3], net_config->mac[4], net_config->mac[5]);

    kprintf("%i virtqueue pairs",net_config->max_virtqueue_pairs);
    kprintf("%x speed", net_config->speed);
    kprintf("status = %x", net_config->status);

    uint8_t dest[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    kprintf("%x:%x:%x:%x:%x:%x", dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
    
    size_t payload_size = 6;
    char hw[6] = {'h','e','l','l','o', '\0'};

    select_queue(&vnp_net_dev, RECEIVE_QUEUE);

    for (uint16_t i = 0; i < 128; i++){
        void* buf = allocate_in_page(vnp_net_dev.memory_page, MAX_PACKET_SIZE, ALIGN_64B, true, true);
        virtio_add_buffer(&vnp_net_dev, i, (uintptr_t)buf, MAX_PACKET_SIZE);
    }
    
    void* test_packet = allocate_in_page(vnp_net_dev.memory_page, UDP_PACKET_SIZE + payload_size, ALIGN_64B, true, true);
    create_udp_packet(test_packet,net_config->mac,dest,(192 << 24) | (168 << 16) | (1 << 8) | 131,(255 << 24) | (255 << 16) | (255 << 8) | 255,8888,8080,hw, payload_size);

    kprintf("Packet created");

    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);

    size_t size = UDP_PACKET_SIZE + payload_size;
    
    kprintf("Upload queue selected for packet %x (size %i)",(uintptr_t)test_packet, size);

    virtio_send_1d(&vnp_net_dev, (uintptr_t)test_packet, size);

    kprintf("Packet sent");

    return true;
}
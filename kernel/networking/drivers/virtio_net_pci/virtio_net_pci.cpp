#include "virtio_net_pci.hpp"
#include "console/kio.h"
#include "networking/network.h"
#include "pci.h"
#include "syscalls/syscalls.h"
#include "memory/page_allocator.h"
#include "std/memfunctions.h"

#define RECEIVE_QUEUE 0
#define TRANSMIT_QUEUE 1

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

typedef struct __attribute__((packed)) virtio_net_hdr_t {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t num_buffers;
} virtio_net_hdr_t;

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

VirtioNetDriver* VirtioNetDriver::try_init(){
    VirtioNetDriver* driver = new VirtioNetDriver();
    if (driver->init())
        return driver;
    delete driver;
    return nullptr;
}

bool VirtioNetDriver::init(){
    uint64_t addr = find_pci_device(VIRTIO_VENDOR, VIRTIO_NET_ID);
    if (!addr){ 
        kprintf("[VIRTIO_NET error] Virtio network device not found");
        return false;
    }
    
    uint64_t net_device_address, net_device_size;
    
    kprintfv("[VIRTIO_NET] Configuring network device");
    
    virtio_get_capabilities(&vnp_net_dev, addr, &net_device_address, &net_device_size);
    pci_register(net_device_address, net_device_size);

    uint8_t interrupts_ok = pci_setup_interrupts(addr, NET_IRQ, 2);
    switch(interrupts_ok){
        case 0:
            kprintf("[VIRTIO_NET] Failed to setup interrupts");
            return false;
        case 1:
            kprintf("[VIRTIO_NET] Interrupts setup with MSI-X %i, %i",NET_IRQ,NET_IRQ+1);
            break;
        default:
            kprintf("[VIRTIO_NET] Interrupts setup with MSI %i,%i",NET_IRQ,NET_IRQ+1);
            break;
    }
    pci_enable_device(addr);

    if (!virtio_init_device(&vnp_net_dev)) {
        kprintf("[VIRTIO_NET error] Failed network initialization");
        return false;
    }
    kprintf("[VIRTIO_NET] Device set up at %x",(uintptr_t)vnp_net_dev.device_cfg);

    select_queue(&vnp_net_dev, RECEIVE_QUEUE);

    for (uint16_t i = 0; i < 128; i++){
        void* buf = kalloc(vnp_net_dev.memory_page, MAX_PACKET_SIZE, ALIGN_64B, true, true);
        virtio_add_buffer(&vnp_net_dev, i, (uintptr_t)buf, MAX_PACKET_SIZE);
    }

    vnp_net_dev.common_cfg->queue_msix_vector = 0;
    if (vnp_net_dev.common_cfg->queue_msix_vector != 0){
        kprintf("[VIRTIO_NET error] failed to set interrupts on receive queue, network will be unable to receive packets");
        return false;
    }

    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);

    vnp_net_dev.common_cfg->queue_msix_vector = 1;
    if (vnp_net_dev.common_cfg->queue_msix_vector != 1){
        kprintf("[VIRTIO_NET error] failed to set interrupts on transmit queue, network will be unable to cleanup transmitted packets");
        return false;
    }

    header_size = sizeof(virtio_net_hdr_t);

    return true;
}


void VirtioNetDriver::get_mac(network_connection_ctx *context){
    virtio_net_config* net_config = (virtio_net_config*)vnp_net_dev.device_cfg;
    kprintfv("[VIRTIO_NET] %x:%x:%x:%x:%x:%x", net_config->mac[0], net_config->mac[1], net_config->mac[2], net_config->mac[3], net_config->mac[4], net_config->mac[5]);

    memcpy((void*)&context->mac,(void*)&net_config->mac,6);
    
    kprintfv("[VIRTIO_NET] %i virtqueue pairs",net_config->max_virtqueue_pairs);
    kprintfv("[VIRTIO_NET] %x speed", net_config->speed);
    kprintfv("[VIRTIO_NET] status = %x", net_config->status);
}

sizedptr VirtioNetDriver::allocate_packet(size_t size){
    return (sizedptr){(uintptr_t)kalloc(vnp_net_dev.memory_page, size + header_size, ALIGN_64B, true, true),size + header_size};
}

sizedptr VirtioNetDriver::handle_receive_packet(void* buffer){
    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    struct virtq_used* used = (struct virtq_used*)(uintptr_t)vnp_net_dev.common_cfg->queue_device;
    struct virtq_desc* desc = (struct virtq_desc*)(uintptr_t)vnp_net_dev.common_cfg->queue_desc;
    struct virtq_avail* avail = (struct virtq_avail*)(uintptr_t)vnp_net_dev.common_cfg->queue_driver;

    uint16_t new_idx = used->idx;
    if (new_idx != last_used_receive_idx) {
        uint16_t used_ring_index = last_used_receive_idx % 128;
        last_used_receive_idx = new_idx;
        struct virtq_used_elem* e = &used->ring[used_ring_index];
        uint32_t desc_index = e->id;
        uint32_t len = e->len;
        kprintfv("Received network packet %i at index %i (len %i - %i)",used->idx, desc_index, len,sizeof(virtio_net_hdr_t));

        uintptr_t packet = desc[desc_index].addr;
        packet += sizeof(virtio_net_hdr_t);

        memcpy(buffer, (void*)packet, len);

        avail->ring[avail->idx % 128] = desc_index;
        avail->idx++;

        *(volatile uint16_t*)(uintptr_t)(vnp_net_dev.notify_cfg + vnp_net_dev.notify_off_multiplier * RECEIVE_QUEUE) = 0;

        kfree((void*)desc[desc_index].addr, MAX_PACKET_SIZE);

        return (sizedptr){packet,len};
    }

    return (sizedptr){0,0};
}

void VirtioNetDriver::handle_sent_packet(){
    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);

    struct virtq_used* used = (struct virtq_used*)(uintptr_t)vnp_net_dev.common_cfg->queue_device;
    struct virtq_desc* desc = (struct virtq_desc*)(uintptr_t)vnp_net_dev.common_cfg->queue_desc;

    uint16_t new_idx = used->idx;

    if (new_idx != last_used_sent_idx) {
        uint16_t used_ring_index = last_used_sent_idx % 128;
        last_used_sent_idx = new_idx;
        struct virtq_used_elem* e = &used->ring[used_ring_index];
        uint32_t desc_index = e->id;
        uint32_t len = e->len;
        kfree((void*)desc[desc_index].addr, len);
        return;
    }
}

void VirtioNetDriver::send_packet(sizedptr packet){
    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);
    
    if (packet.ptr && packet.size)
        virtio_send_1d(&vnp_net_dev, packet.ptr, packet.size);
    
    kprintfv("Queued new packet");
}

void VirtioNetDriver::enable_verbose(){
    verbose = true;
}

#include "virtio_net_pci.hpp"
#include "console/kio.h"
#include "pci.h"
#include "syscalls/syscalls.h"
#include "memory/page_allocator.h"
#include "std/memory.h"
#include "networking/network.h"
#include "sysregs.h"

#define RECEIVE_QUEUE 0
#define TRANSMIT_QUEUE 1

static constexpr uint32_t RX_BUF_SIZE = PAGE_SIZE;

static void* g_rx_pool = nullptr;
static uint16_t g_rx_qsz = 0;

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })
VirtioNetDriver::VirtioNetDriver() {
    verbose = false;
    header_size = sizeof(virtio_net_hdr_t);
    mtu = 1500;
    speed_mbps = 0xFFFFFFFFu;
    duplex = LINK_DUPLEX_UNKNOWN;
    last_used_receive_idx = 0;
    last_used_sent_idx = 0;
    hw_name[0] = 0;
}

VirtioNetDriver::~VirtioNetDriver(){}

bool VirtioNetDriver::init_at(uint64_t addr, uint32_t irq_base_vector){
    kprintfv("[virtio-net] probing pci_addr=%x",(uintptr_t)addr);

    uint64_t mmio_addr = 0, mmio_size = 0;
    virtio_get_capabilities(&vnp_net_dev, addr, &mmio_addr, &mmio_size);
    kprintfv("[virtio-net] mmio=%x size=%x",(uintptr_t)mmio_addr,(uintptr_t)mmio_size);

    if (vnp_net_dev.common_cfg) pci_register(((uintptr_t)vnp_net_dev.common_cfg) & ~(uintptr_t)(PAGE_SIZE-1), PAGE_SIZE);
    if (vnp_net_dev.device_cfg) pci_register(((uintptr_t)vnp_net_dev.device_cfg) & ~(uintptr_t)(PAGE_SIZE-1), PAGE_SIZE);
    if (vnp_net_dev.notify_cfg) pci_register(((uintptr_t)vnp_net_dev.notify_cfg) & ~(uintptr_t)(PAGE_SIZE-1), PAGE_SIZE);
    if (vnp_net_dev.isr_cfg) pci_register(((uintptr_t)vnp_net_dev.isr_cfg) & ~(uintptr_t)(PAGE_SIZE-1), PAGE_SIZE);

    uint8_t interrupts_ok = pci_setup_interrupts(addr, irq_base_vector, 2);
    if (!interrupts_ok){
        kprintf("[virtio-net][err] pci_setup_interrupts failed");
        return false;
    }
    if (interrupts_ok == 1){
        kprintfv("[virtio-net] interrupts MSI-X base=%u",(unsigned)irq_base_vector);
    } else {
        kprintfv("[virtio-net] interrupts MSI base=%u",(unsigned)irq_base_vector);
    }
    pci_enable_device(addr);
    kprintfv("[virtio-net] device enabled");

    if (!virtio_init_device(&vnp_net_dev)){
        kprintf("[virtio-net][err] virtio_init_device failed");
        return false;
    }
    kprintfv("[virtio-net] common_cfg=%x device_cfg=%x", (uintptr_t)vnp_net_dev.common_cfg,(uintptr_t)vnp_net_dev.device_cfg);

    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    uint16_t rx_qsz = vnp_net_dev.common_cfg->queue_size;
    if (!rx_qsz) return false;
    g_rx_qsz = rx_qsz;
    kprintfv("[virtio-net] RX qsz=%u",rx_qsz);

    if (!g_rx_pool){
        g_rx_pool = palloc((uint64_t)rx_qsz * RX_BUF_SIZE, MEM_PRIV_KERNEL, MEM_RW, true);
        kprintfv("[virtio-net] rx_pool=%x",(uintptr_t)g_rx_pool);
        if (!g_rx_pool) return false;
    }
    for (uint16_t i=0;i<rx_qsz;i++){
        void* buf = (void*)((uintptr_t)g_rx_pool + (uintptr_t)i * RX_BUF_SIZE);
        virtio_add_buffer(&vnp_net_dev, i, (uintptr_t)buf, RX_BUF_SIZE, false);
    }

    vnp_net_dev.common_cfg->queue_msix_vector = 0;
    kprintfv("[virtio-net] RX vector=%u",vnp_net_dev.common_cfg->queue_msix_vector);
    if (vnp_net_dev.common_cfg->queue_msix_vector != 0) return false;

    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);
    vnp_net_dev.common_cfg->queue_msix_vector = 1;
    kprintfv("[virtio-net] TX vector=%u",vnp_net_dev.common_cfg->queue_msix_vector);
    if (vnp_net_dev.common_cfg->queue_msix_vector != 1) return false;

    virtio_net_config* cfg = (virtio_net_config*)vnp_net_dev.device_cfg;

    uint8_t mac[6]; get_mac(mac);


    uint16_t dev_mtu = cfg->mtu;
    if (dev_mtu != 0 && dev_mtu != 0xFFFF && dev_mtu >= 576) mtu = dev_mtu; else mtu = 1500;

    header_size = sizeof(virtio_net_hdr_t);

    speed_mbps = cfg->speed;
    switch (cfg->duplex) {
        case 0: duplex = LINK_DUPLEX_HALF; break;
        case 1: duplex = LINK_DUPLEX_FULL; break;
        default: duplex = LINK_DUPLEX_UNKNOWN; break;
    }

    hw_name[0] = 'v'; hw_name[1] = 'i'; hw_name[2] = 'r'; hw_name[3] = 't'; hw_name[4] = 'i'; hw_name[5] = 'o'; hw_name[6] = 0;

    const char* dpx_str = (duplex == LINK_DUPLEX_FULL) ? "full" : (duplex == LINK_DUPLEX_HALF) ? "half" : "unknown";
    if (speed_mbps != 0xFFFFFFFF) {
        kprintfv("[virtio-net] mac=%x:%x:%x:%x:%x:%x mtu=%u hdr=%u speed=%uMbps duplex=%s",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned)mtu, (unsigned)header_size, (unsigned)speed_mbps, dpx_str);
    } else {
        kprintfv("[virtio-net] mac=%x:%x:%x:%x:%x:%x mtu=%u hdr=%u speed=unknown duplex=%s",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned)mtu, (unsigned)header_size, dpx_str);
    }

    return true;
}



void VirtioNetDriver::get_mac(uint8_t out_mac[6]) const {
    virtio_net_config* net_config = (virtio_net_config*)vnp_net_dev.device_cfg;
    memcpy(out_mac, net_config->mac, 6);
}

uint16_t VirtioNetDriver::get_mtu() const {
    return mtu;
}

uint16_t VirtioNetDriver::get_header_size() const {
    return header_size;
}

const char* VirtioNetDriver::hw_ifname() const {
    return hw_name;
}

uint32_t VirtioNetDriver::get_speed_mbps() const { return speed_mbps; }

uint8_t VirtioNetDriver::get_duplex() const {
    switch (duplex) {
        case LINK_DUPLEX_HALF: return 0;
        case LINK_DUPLEX_FULL: return 1;
        default: return 0xFF;
    }
}

sizedptr VirtioNetDriver::allocate_packet(size_t size){
    return (sizedptr){(uintptr_t)kalloc(vnp_net_dev.memory_page, size + header_size, ALIGN_64B, MEM_PRIV_KERNEL), size + header_size};
}

sizedptr VirtioNetDriver::handle_receive_packet(){
    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    volatile virtq_used* used = (volatile virtq_used*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_device);
    volatile virtq_desc* desc = (volatile virtq_desc*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_desc);
    volatile virtq_avail* avail = (volatile virtq_avail*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_driver);

    uint16_t qsz = vnp_net_dev.common_cfg->queue_size;
    uint16_t new_idx = used->idx;
    if (new_idx == last_used_receive_idx) {
        return (sizedptr){0,0};
    }

    uint16_t used_ring_index = (uint16_t)(last_used_receive_idx % qsz);
    volatile virtq_used_elem* e = &used->ring[used_ring_index];
    last_used_receive_idx++;

    uint32_t desc_index = e->id;
    uint32_t len = e->len;
    if (desc_index >= qsz || len <= header_size){
        uint16_t aidx = avail->idx;
        avail->ring[aidx % qsz] = (uint16_t)desc_index;
        asm volatile ("dmb ishst" ::: "memory");
        avail->idx = (uint16_t)(aidx + 1);
        asm volatile ("dmb ishst" ::: "memory");
        *(volatile uint16_t*)(vnp_net_dev.notify_cfg + vnp_net_dev.notify_off_multiplier * vnp_net_dev.common_cfg->queue_notify_off) = 0;
        return (sizedptr){0,0};
    }

    uintptr_t packet_addr = (uintptr_t)PHYS_TO_VIRT_P((void*)(uintptr_t)desc[desc_index].addr);
    uint32_t payload_len = len - header_size;

    void* out_buf = kalloc(vnp_net_dev.memory_page, payload_len, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!out_buf){
        uint16_t aidx = avail->idx;
        avail->ring[aidx % qsz] = (uint16_t)desc_index;
        asm volatile ("dmb ishst" ::: "memory");
        avail->idx = (uint16_t)(aidx + 1);
        asm volatile ("dmb ishst" ::: "memory");
        *(volatile uint16_t*)(vnp_net_dev.notify_cfg + vnp_net_dev.notify_off_multiplier * vnp_net_dev.common_cfg->queue_notify_off) = 0;
        return (sizedptr){0,0};
    }
    memcpy(out_buf, (void*)(packet_addr + header_size), payload_len);

    uint16_t aidx = avail->idx;
    avail->ring[aidx % qsz] = (uint16_t)desc_index;
    asm volatile ("dmb ishst" ::: "memory");
    avail->idx = (uint16_t)(aidx + 1);
    asm volatile ("dmb ishst" ::: "memory");
    *(volatile uint16_t*)(vnp_net_dev.notify_cfg + vnp_net_dev.notify_off_multiplier * vnp_net_dev.common_cfg->queue_notify_off) = 0;

    return (sizedptr){ (uintptr_t)out_buf, payload_len };
}

void VirtioNetDriver::handle_sent_packet(){
    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);

    volatile virtq_used* used = (volatile virtq_used*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_device);
    last_used_sent_idx = used->idx;
}

bool VirtioNetDriver::send_packet(sizedptr packet){
    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);
    if (!packet.ptr || !packet.size) return false;
    if (header_size <= packet.size) memset((void*)packet.ptr, 0, header_size);
    bool ok = virtio_send_1d(&vnp_net_dev, packet.ptr, packet.size);
    kprintfv("[virtio-net] tx queued len=%u",(unsigned)packet.size);
    kfree((void*)packet.ptr, packet.size);
    return ok;
}

void VirtioNetDriver::enable_verbose(){
    verbose = true;
}
#include "virtio_net_pci.hpp"
#include "console/kio.h"
#include "pci.h"
#include "syscalls/syscalls.h"
#include "memory/page_allocator.h"
#include "std/memory.h"
#include "networking/network.h"
#include "sysregs.h"
#include "exceptions/irq.h"

#define RECEIVE_QUEUE 0
#define TRANSMIT_QUEUE 1
#define CONTROL_QUEUE 2

constexpr uint32_t RX_BUF_SIZE = PAGE_SIZE;
constexpr uint16_t RX_CHAIN_SEGS = 1;
constexpr uint64_t RX_PACKET_POOL_SIZE = 8*1024*1024;
constexpr uint64_t TX_PACKET_POOL_SIZE = 8*1024*1024;

void* g_rx_pool = nullptr;
void* g_rx_packet_pool = nullptr;
void* g_tx_packet_pool = nullptr;

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })
   
typedef struct __attribute__((packed)) virtio_net_ctrl_hdr_t {
    uint8_t cls;
    uint8_t cmd;
} virtio_net_ctrl_hdr_t;

typedef struct __attribute__((packed)) virtio_net_ctrl_ack_t {
    uint8_t ack;
} virtio_net_ctrl_ack_t;

#define VIRTIO_NET_CTRL_RX 0
#define VIRTIO_NET_CTRL_MAC 1

#define VIRTIO_NET_CTRL_RX_PROMISC 0
#define VIRTIO_NET_CTRL_RX_ALLMULTI 1
#define VIRTIO_NET_CTRL_RX_NOMULTI 3

#define VIRTIO_NET_CTRL_MAC_TABLE_SET 0


bool virtio_net_ctrl_send(virtio_device* dev, uint8_t cls, uint8_t cmd, const void* payload, uint32_t payload_len) {
    if (!dev) return false;

    virtio_net_ctrl_hdr_t hdr;
    hdr.cls= cls;
    hdr.cmd = cmd;

    uint32_t in_len = (uint32_t)sizeof(hdr) + payload_len;
    uint8_t* in = (uint8_t*)kalloc(dev->memory_page, (size_t)in_len, ALIGN_16B, MEM_PRIV_KERNEL);
    if (!in) return false;
    memcpy(in, &hdr, sizeof(hdr));
    if (payload_len && payload) memcpy(in + sizeof(hdr), payload, payload_len);

    virtio_net_ctrl_ack_t* ack = (virtio_net_ctrl_ack_t*)kalloc(dev->memory_page, sizeof(virtio_net_ctrl_ack_t), ALIGN_16B, MEM_PRIV_KERNEL);
    if (!ack) {
        kfree(in, in_len);
        return false;
    }
    ack->ack = 1;

    virtio_buf bufs[2];
    bufs[0] = VBUF((uintptr_t)in, in_len, 0);
    bufs[1] = VBUF((uintptr_t)ack, sizeof(virtio_net_ctrl_ack_t), VIRTQ_DESC_F_WRITE);

    select_queue(dev, CONTROL_QUEUE);
    bool ok = virtio_send_nd(dev, bufs, 2);
    bool aok = (ack->ack == 0);

    kfree(ack, sizeof(virtio_net_ctrl_ack_t));
    kfree(in, in_len);
    return ok && aok;
}

VirtioNetDriver::VirtioNetDriver() {
    hw_name[0] = 0;
}

VirtioNetDriver::~VirtioNetDriver(){}

bool VirtioNetDriver::init_at(uint64_t addr, uint32_t irq_base_vector) {
    verbose = false;
    ctrl_vq = false;
    ctrl_rx = false;
    header_size = sizeof(virtio_net_hdr_t);
    mtu = 1500;
    speed_mbps = 0xFFFFFFFFu;
    duplex = LINK_DUPLEX_UNKNOWN;
    last_used_receive_idx = 0;
    last_used_sent_idx = 0;
    rx_desc = nullptr;
    rx_avail = nullptr;
    rx_used = nullptr;
    rx_qsz = 0;
    tx_desc = nullptr;
    tx_avail = nullptr;
    tx_used = nullptr;
    tx_qsz = 0;
    tx_next_desc = 0;
    tx_pending = nullptr;
    tx_pending_size = 0;
    memset(&vnp_net_dev, 0, sizeof(vnp_net_dev));

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

    uint64_t net_feature_mask = 0;
    net_feature_mask |= (1ULL << VIRTIO_F_VERSION_1);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_MAC);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_STATUS);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_MTU);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_CTRL_VQ);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_CTRL_RX);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_SPEED_DUPLEX);
    //TODO evaluate MRG_RXBUF CSUM TSO GSO GRO USO
    virtio_set_feature_mask(net_feature_mask);

    if (!virtio_init_device(&vnp_net_dev)){
        kprintf("[virtio-net][err] virtio_init_device failed");
        return false;
    }
    kprintfv("[virtio-net] common_cfg=%x device_cfg=%x", (uintptr_t)vnp_net_dev.common_cfg,(uintptr_t)vnp_net_dev.device_cfg);

    if (!(vnp_net_dev.negotiated_features & (1ULL << VIRTIO_F_VERSION_1))) {
        kprintf("[virtio-net][err] device did not accept VIRTIO_F_VERSION_1");
        vnp_net_dev.common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return false;
    }
    header_size = sizeof(virtio_net_hdr_t);

    ctrl_vq = (vnp_net_dev.negotiated_features & (1ULL << VIRTIO_NET_F_CTRL_VQ)) != 0;
    ctrl_rx = (vnp_net_dev.negotiated_features & (1ULL << VIRTIO_NET_F_CTRL_RX)) != 0;
    if (CONTROL_QUEUE >= vnp_net_dev.num_queues || !vnp_net_dev.queues[CONTROL_QUEUE].valid || !vnp_net_dev.queues[CONTROL_QUEUE].size) {
        ctrl_vq = false;
        ctrl_rx = false;
    }

    if (RECEIVE_QUEUE >= vnp_net_dev.num_queues) return false;
    if (!vnp_net_dev.queues[RECEIVE_QUEUE].valid) return false;

    rx_qsz = vnp_net_dev.queues[RECEIVE_QUEUE].size;
    rx_desc = vnp_net_dev.queues[RECEIVE_QUEUE].desc;
    rx_avail = vnp_net_dev.queues[RECEIVE_QUEUE].driver;
    rx_used = vnp_net_dev.queues[RECEIVE_QUEUE].device;

    if (!rx_qsz || !rx_desc || !rx_avail || !rx_used) return false;
    kprintfv("[virtio-net] RX qsz=%u",rx_qsz);

    if (!g_rx_pool){
        g_rx_pool = palloc((uint64_t)rx_qsz * RX_BUF_SIZE, MEM_PRIV_KERNEL, MEM_RW, true);
        kprintfv("[virtio-net] rx_pool=%x",(uintptr_t)g_rx_pool);
        if (!g_rx_pool) return false;
    }

    if (!g_rx_packet_pool) {
        g_rx_packet_pool = palloc(RX_PACKET_POOL_SIZE, MEM_PRIV_KERNEL, MEM_RW | MEM_NORM, false);
        kprintfv("[virtio-net] rx_packet_pool=%x", (uintptr_t)g_rx_packet_pool);
        if (!g_rx_packet_pool) return false;
    }

    if (!g_tx_packet_pool) {
        g_tx_packet_pool = palloc(TX_PACKET_POOL_SIZE, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, false);
        kprintfv("[virtio-net] tx_packet_pool=%x", (uintptr_t)g_tx_packet_pool);
        if (!g_tx_packet_pool) return false;
    }

    uint16_t chain_count = (uint16_t)(rx_qsz / RX_CHAIN_SEGS);
    if (!chain_count) return false;

    memset((void*)rx_desc, 0, 16ULL * rx_qsz);
    rx_avail->flags = 0;
    rx_avail->idx = 0;
    rx_used->flags = 0;
    rx_used->idx = 0;

    for (uint16_t c = 0; c < chain_count; c++) {
        uint16_t di = (uint16_t)(c * RX_CHAIN_SEGS);
        void* buf = (void*)((uintptr_t)g_rx_pool + (uintptr_t)di * (uintptr_t)RX_BUF_SIZE);

        rx_desc[di].addr = VIRT_TO_PHYS((uintptr_t)buf);
        rx_desc[di].len = RX_BUF_SIZE;
        rx_desc[di].flags = VIRTQ_DESC_F_WRITE;
        rx_desc[di].next = 0;

        rx_avail->ring[rx_avail->idx % rx_qsz] = di;
        rx_avail->idx++;
    }

    asm volatile ("dmb ishst" ::: "memory");

    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    vnp_net_dev.common_cfg->queue_msix_vector = 0;
    kprintfv("[virtio-net] RX vector=%u",vnp_net_dev.common_cfg->queue_msix_vector);
    if (vnp_net_dev.common_cfg->queue_msix_vector != 0) return false;

    if (TRANSMIT_QUEUE >= vnp_net_dev.num_queues) return false;
    if (!vnp_net_dev.queues[TRANSMIT_QUEUE].valid) return false;

    tx_qsz = vnp_net_dev.queues[TRANSMIT_QUEUE].size;
    tx_desc = vnp_net_dev.queues[TRANSMIT_QUEUE].desc;
    tx_avail = vnp_net_dev.queues[TRANSMIT_QUEUE].driver;
    tx_used = vnp_net_dev.queues[TRANSMIT_QUEUE].device;
    if (!tx_qsz || !tx_desc || !tx_avail || !tx_used) return false;

    tx_pending_size = (size_t)tx_qsz * sizeof(sizedptr);
    tx_pending = (sizedptr*)kalloc(vnp_net_dev.memory_page, tx_pending_size, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!tx_pending) return false;

    memset((void*)tx_desc, 0, 16 * tx_qsz);
    tx_avail->flags = 0;
    tx_avail->idx = 0;
    tx_used->flags = 0;
    tx_used->idx = 0;
    last_used_sent_idx = 0;
    tx_next_desc = 0;

    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);
    vnp_net_dev.common_cfg->queue_msix_vector = 1;
    kprintfv("[virtio-net] TX vector=%u",vnp_net_dev.common_cfg->queue_msix_vector);
    if (vnp_net_dev.common_cfg->queue_msix_vector != 1) return false;

    if (ctrl_vq) {
        select_queue(&vnp_net_dev, CONTROL_QUEUE);
        vnp_net_dev.common_cfg->queue_msix_vector = 0xFFFF;
    }

    kprintfv("[virtio-net] negotiated ctrl_vq=%u ctrl_rx=%u", (unsigned)ctrl_vq, (unsigned)ctrl_rx);

    volatile virtio_net_config* cfg = (volatile virtio_net_config*)vnp_net_dev.device_cfg;

    uint8_t mac[6];
    get_mac(mac);


    if (vnp_net_dev.negotiated_features & (1ULL << VIRTIO_NET_F_MTU)) {
        uint16_t dev_mtu = cfg->mtu;
        if (dev_mtu != 0 && dev_mtu != 0xFFFF && dev_mtu >= 576) mtu = dev_mtu;
    }

    if (vnp_net_dev.negotiated_features & (1ULL << VIRTIO_NET_F_SPEED_DUPLEX)) {
        speed_mbps = cfg->speed;
        switch (cfg->duplex) {
            case 0: duplex = LINK_DUPLEX_HALF; break;
            case 1: duplex = LINK_DUPLEX_FULL; break;
            default: duplex = LINK_DUPLEX_UNKNOWN; break;
        }
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

    vnp_net_dev.common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    asm volatile ("dsb sy" ::: "memory");

    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    virtio_notify(&vnp_net_dev);
    if (ctrl_vq && ctrl_rx) (void)sync_multicast((const uint8_t*)0, 0);
    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    return true;
}



void VirtioNetDriver::get_mac(uint8_t out_mac[6]) const {
    volatile virtio_net_config* net_config = (volatile virtio_net_config*)vnp_net_dev.device_cfg;
    memcpy(out_mac, (const void*)net_config->mac, 6);
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
    if (!g_tx_packet_pool) return (sizedptr){0, 0};
    size_t total = size + header_size;

    irq_flags_t iqr = irq_save_disable();
    void* ptr = kalloc(g_tx_packet_pool, total, ALIGN_64B, MEM_PRIV_KERNEL);
    irq_restore(iqr);

    return (sizedptr){(uintptr_t)ptr, total};
}

sizedptr VirtioNetDriver::handle_receive_packet(){
    if (!rx_qsz || !rx_desc || !rx_used || !rx_avail) return (sizedptr){0,0};

    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    asm volatile ("dmb ishld" ::: "memory");

    uint16_t new_idx = rx_used->idx;
    if (new_idx == last_used_receive_idx) return (sizedptr){0,0};

    uint16_t used_ring_index = (uint16_t)(last_used_receive_idx % rx_qsz);
    volatile virtq_used_elem* e = &rx_used->ring[used_ring_index];
    uint32_t desc_index = e->id;
    uint32_t total_len = e->len;
    last_used_receive_idx++;

    if (desc_index >= rx_qsz) return (sizedptr){0,0};
    if (total_len <= (uint32_t)header_size || total_len > rx_desc[desc_index].len) {
        uint16_t aidx = rx_avail->idx;
        rx_avail->ring[aidx % rx_qsz] = (uint16_t)desc_index;
        asm volatile ("dmb ishst" ::: "memory");
        rx_avail->idx = (uint16_t)(aidx + 1);
        asm volatile ("dmb ishst" ::: "memory");
        virtio_notify(&vnp_net_dev);
        return (sizedptr){0,0};
    }

    uint32_t payload_len = total_len - (uint32_t)header_size;
    volatile uint8_t* buf = (volatile uint8_t*)PHYS_TO_VIRT_P((void*)rx_desc[desc_index].addr);

    void* out_buf = nullptr;
    if (g_rx_packet_pool) {
        irq_flags_t irq = irq_save_disable();
        out_buf = kalloc(g_rx_packet_pool, payload_len, ALIGN_64B, MEM_PRIV_KERNEL);
        irq_restore(irq);
    }
    if (!out_buf){
        uint16_t aidx = rx_avail->idx;
        rx_avail->ring[aidx % rx_qsz] = (uint16_t)desc_index;
        asm volatile ("dmb ishst" ::: "memory");
        rx_avail->idx = (uint16_t)(aidx + 1);
        asm volatile ("dmb ishst" ::: "memory");
        virtio_notify(&vnp_net_dev);
        return (sizedptr){0,0};
    }

    memcpy(out_buf, (const void*)((uintptr_t)buf + (uintptr_t)header_size), payload_len);

    uint16_t aidx = rx_avail->idx;
    rx_avail->ring[aidx % rx_qsz] = (uint16_t)desc_index;
    asm volatile ("dmb ishst" ::: "memory");
    rx_avail->idx = (uint16_t)(aidx + 1);
    asm volatile ("dmb ishst" ::: "memory");
    virtio_notify(&vnp_net_dev);

    return (sizedptr){ (uintptr_t)out_buf, payload_len };
}

void VirtioNetDriver::handle_sent_packet(){
    if (TRANSMIT_QUEUE >= vnp_net_dev.num_queues) return;
    if (!tx_qsz || !tx_desc || !tx_used || !tx_pending) return;

    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);
    asm volatile ("dmb ishld" ::: "memory");

    uint16_t used_idx = tx_used->idx;
    while (last_used_sent_idx != used_idx) {
        uint16_t ring_index = (uint16_t)(last_used_sent_idx % tx_qsz);
        uint16_t desc_index = (uint16_t)tx_used->ring[ring_index].id;
        last_used_sent_idx++;

        if (desc_index >= tx_qsz) continue;

        sizedptr packet = tx_pending[desc_index];
        tx_pending[desc_index] = (sizedptr){0,0};
        tx_desc[desc_index].addr = 0;
        tx_desc[desc_index].len = 0;
        tx_desc[desc_index].flags = 0;
        tx_desc[desc_index].next = 0;

        if (packet.ptr) {
            irq_flags_t irq = irq_save_disable();
            kfree((void*)packet.ptr, packet.size);
            irq_restore(irq);
        }
        asm volatile ("dmb ishld" ::: "memory");
        used_idx = tx_used->idx;
    }
}

bool VirtioNetDriver::send_packet(sizedptr packet){
    if (!packet.ptr || !packet.size) return false;
    if ((size_t)header_size > packet.size) return false;
    if (packet.size > 0xFFFFFFFF) return false;
    if (!tx_qsz || !tx_desc || !tx_avail || !tx_used || !tx_pending) return false;

    memset((void*)packet.ptr, 0, (size_t)header_size);

    handle_sent_packet();
    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);

    bool found = false;
    uint16_t desc_index = 0;
    for (uint16_t i = 0; i < tx_qsz; i++) {
        uint16_t current = (tx_next_desc + i) % tx_qsz;
        if (tx_pending[current].ptr) continue;
        desc_index = current;
        tx_next_desc = (current + 1) % tx_qsz;
        found = true;
        break;
    }

    if (!found) {
        kprintfv("[virtio-net] tx queue len=%u", packet.size);
        return false;
    }

    tx_pending[desc_index] = packet;
    tx_desc[desc_index].addr = VIRT_TO_PHYS(packet.ptr);
    tx_desc[desc_index].len = (uint32_t)packet.size;
    tx_desc[desc_index].flags = 0;
    tx_desc[desc_index].next = 0;

    asm volatile ("dmb ishst" ::: "memory");
    uint16_t avail_idx = tx_avail->idx;
    tx_avail->ring[avail_idx % tx_qsz] = desc_index;
    asm volatile ("dmb ishst" ::: "memory");
    tx_avail->idx = (uint16_t)(avail_idx + 1);
    asm volatile ("dmb ishst" ::: "memory");
    virtio_notify(&vnp_net_dev);

    kprintfv("[virtio-net] tx queued desc=%u len=%u", desc_index,(unsigned)packet.size);
    return true;
}

bool VirtioNetDriver::sync_multicast(const uint8_t* macs, uint32_t count) {
    if (!ctrl_vq) return true;
    if (!ctrl_rx) return true;
    if (!macs && count) return false;

    bool ok = true;

    uint8_t v0 = 0;
    uint8_t v1 = 1;

    ok = ok && virtio_net_ctrl_send(&vnp_net_dev, VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_PROMISC, &v0, 1);
    ok = ok && virtio_net_ctrl_send(&vnp_net_dev, VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_ALLMULTI, &v0, 1);

    if (count == 0) ok = ok && virtio_net_ctrl_send(&vnp_net_dev, VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_NOMULTI, &v1, 1);
    else ok = ok && virtio_net_ctrl_send(&vnp_net_dev, VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_NOMULTI, &v0, 1);

    uint32_t payload_len = 8u + count * 6u;
    uint8_t* payload = (uint8_t*)kalloc(vnp_net_dev.memory_page, payload_len, ALIGN_16B, MEM_PRIV_KERNEL);
    if (!payload) {
        return false;
    }
    kprintfv("[virtio-net] sync_multicast ctrl_vq=%u ctrl_rx=%u count=%u",(unsigned)ctrl_vq, (unsigned)ctrl_rx, (unsigned)count);

    uint32_t uc = 0;
    memcpy(payload + 0, &uc, 4);
    memcpy(payload + 4, &count, 4);
    for (uint32_t i = 0; i < count; ++i) memcpy(payload + 8u + i * 6u, macs + i * 6u, 6);

    ok = ok && virtio_net_ctrl_send(&vnp_net_dev, VIRTIO_NET_CTRL_MAC, VIRTIO_NET_CTRL_MAC_TABLE_SET, payload, payload_len);

    kfree(payload, payload_len);
    return ok;
}

void VirtioNetDriver::enable_verbose(){
    verbose = true;
}
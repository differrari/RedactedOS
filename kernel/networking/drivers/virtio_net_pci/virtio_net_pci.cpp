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

static constexpr uint32_t RX_BUF_SIZE = PAGE_SIZE;
static constexpr uint16_t RX_CHAIN_SEGS = 4;

static void* g_rx_pool = nullptr;
static uint16_t g_rx_qsz = 0;

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


static bool virtio_net_ctrl_send(virtio_device* dev, uint8_t cls, uint8_t cmd, const void* payload, uint32_t payload_len) {
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
    verbose = false;
    mrg_rxbuf = false;
    ctrl_vq = false;
    ctrl_rx = false;
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

    uint64_t net_feature_mask = 0;
    net_feature_mask |= (1ULL << VIRTIO_F_VERSION_1);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_MAC);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_STATUS);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_MTU);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_MRG_RXBUF);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_CTRL_VQ);
    net_feature_mask |= (1ULL << VIRTIO_NET_F_CTRL_RX);
    virtio_set_feature_mask(net_feature_mask);

    if (!virtio_init_device(&vnp_net_dev)){
        kprintf("[virtio-net][err] virtio_init_device failed");
        return false;
    }
    kprintfv("[virtio-net] common_cfg=%x device_cfg=%x", (uintptr_t)vnp_net_dev.common_cfg,(uintptr_t)vnp_net_dev.device_cfg);

    mrg_rxbuf = (vnp_net_dev.negotiated_features & (1ULL << VIRTIO_NET_F_MRG_RXBUF)) != 0;
    header_size = mrg_rxbuf ? sizeof(virtio_net_hdr_mrg_rxbuf_t) : sizeof(virtio_net_hdr_t);

    ctrl_vq = (vnp_net_dev.negotiated_features & (1ULL << VIRTIO_NET_F_CTRL_VQ)) != 0;
    ctrl_rx = (vnp_net_dev.negotiated_features & (1ULL << VIRTIO_NET_F_CTRL_RX)) != 0;
    if (ctrl_vq) {
        select_queue(&vnp_net_dev, CONTROL_QUEUE);
        if (!vnp_net_dev.common_cfg->queue_size) {
            ctrl_vq = false;
            ctrl_rx = false;
        } else {
            vnp_net_dev.common_cfg->queue_msix_vector = 0xFFFF;
        }
    }

    if (ctrl_vq && ctrl_rx) (void)sync_multicast((const uint8_t*)0, 0);
    kprintfv("[virtio-net] negotiated ctrl_vq=%u ctrl_rx=%u", (unsigned)ctrl_vq, (unsigned)ctrl_rx);

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
    volatile virtq_desc* rx_desc = (volatile virtq_desc*)PHYS_TO_VIRT_P((virtq_desc*)vnp_net_dev.common_cfg->queue_desc);
    volatile virtq_avail* rx_avail = (volatile virtq_avail*)PHYS_TO_VIRT_P((virtq_avail*)vnp_net_dev.common_cfg->queue_driver);

    uint16_t chain_count = (uint16_t)(rx_qsz / RX_CHAIN_SEGS);
    if (!chain_count) return false;

    rx_avail->idx = 0;

    for (uint16_t c = 0; c < chain_count; c++) {
        uint16_t head = (uint16_t)(c * RX_CHAIN_SEGS);

        for (uint16_t s = 0; s < RX_CHAIN_SEGS; s++){
            uint16_t di = (uint16_t)(head + s);
            void* buf = (void*)((uintptr_t)g_rx_pool + (uintptr_t)di * (uintptr_t)RX_BUF_SIZE);

            rx_desc[di].addr = VIRT_TO_PHYS((uintptr_t)buf);
            rx_desc[di].len = RX_BUF_SIZE;
            rx_desc[di].flags = (uint16_t)(VIRTQ_DESC_F_WRITE | ((s + 1 < RX_CHAIN_SEGS) ? VIRTQ_DESC_F_NEXT : 0));
            rx_desc[di].next = (uint16_t)(di + 1);
        }

        rx_desc[head + (RX_CHAIN_SEGS - 1)].next = 0;

        rx_avail->ring[rx_avail->idx % rx_qsz] = head;
        rx_avail->idx++;
    }

    asm volatile ("dmb ishst" ::: "memory");
    virtio_notify(&vnp_net_dev);

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
    size_t total = size + (size_t)header_size;
    return (sizedptr){(uintptr_t)kalloc(vnp_net_dev.memory_page, total, ALIGN_64B, MEM_PRIV_KERNEL), total};
}

sizedptr VirtioNetDriver::handle_receive_packet(){
    uint32_t desc_index = 0;
    uint32_t total_len = 0;
    uint16_t num_buffers = 1;

    disable_interrupt();
    select_queue(&vnp_net_dev, RECEIVE_QUEUE);
    volatile virtq_used* used = (volatile virtq_used*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_device);
    volatile virtq_desc* desc = (volatile virtq_desc*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_desc);
    volatile virtq_avail* avail = (volatile virtq_avail*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_driver);

    uint16_t qsz = vnp_net_dev.common_cfg->queue_size;
    if (!qsz) {
        enable_interrupt();
        return (sizedptr){0,0};
    }
    asm volatile ("dmb ishld" ::: "memory");

    uint16_t new_idx = used->idx;
    if (new_idx == last_used_receive_idx) {
        enable_interrupt();
        return (sizedptr){0,0};
    }

    uint16_t used_ring_index = (uint16_t)(last_used_receive_idx % qsz);
    volatile virtq_used_elem* e = &used->ring[used_ring_index];
    last_used_receive_idx++;
    desc_index = e->id;
    total_len = e->len;

    if (desc_index >= qsz || total_len <= (uint32_t)header_size){
        uint16_t aidx = avail->idx;
        avail->ring[aidx % qsz] = (uint16_t)(desc_index % qsz);
        asm volatile ("dmb ishst" ::: "memory");
        avail->idx = (uint16_t)(aidx + 1);
        asm volatile ("dmb ishst" ::: "memory");
        virtio_notify(&vnp_net_dev);
        enable_interrupt();
        return (sizedptr){0,0};
    }

    volatile uint8_t* first_buf = (volatile uint8_t*)PHYS_TO_VIRT_P((void*)(uintptr_t)desc[desc_index].addr);
    if (mrg_rxbuf) {
        virtio_net_hdr_mrg_rxbuf_t* h = (virtio_net_hdr_mrg_rxbuf_t*)(uintptr_t)first_buf;
        num_buffers = h->num_buffers;
        if (num_buffers == 0) num_buffers = 1;
        if (num_buffers > RX_CHAIN_SEGS) {
            uint16_t aidx = avail->idx;
            avail->ring[aidx % qsz] = (uint16_t)desc_index;
            asm volatile ("dmb ishst" ::: "memory");
            avail->idx = (uint16_t)(aidx + 1);
            asm volatile ("dmb ishst" ::: "memory");
            virtio_notify(&vnp_net_dev);
            enable_interrupt();
            return (sizedptr){0,0};
        }
    }

    enable_interrupt();

    uint32_t payload_len = total_len - (uint32_t)header_size;
    void* out_buf = kalloc(vnp_net_dev.memory_page, payload_len, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!out_buf){
        disable_interrupt();
        uint16_t aidx = avail->idx;
        avail->ring[aidx % qsz] = (uint16_t)desc_index;
        asm volatile ("dmb ishst" ::: "memory");
        avail->idx = (uint16_t)(aidx + 1);
        asm volatile ("dmb ishst" ::: "memory");
        virtio_notify(&vnp_net_dev);
        enable_interrupt();
        return (sizedptr){0,0};
    }

    uint32_t written = 0;
    uint32_t remaining = payload_len;
    uint16_t di = (uint16_t)desc_index;
    for (uint16_t bi = 0; bi < num_buffers && remaining; bi++) {
        volatile uint8_t* buf = (volatile uint8_t*)PHYS_TO_VIRT_P((void*)(uintptr_t)desc[di].addr);
        uint32_t cap = desc[di].len;
        uint32_t off = (bi == 0) ? (uint32_t)header_size : 0;
        if (cap <= off) break;

        uint32_t chunk = cap - off;
        if (chunk > remaining) chunk = remaining;
        memcpy((uint8_t*)out_buf + written, (const void*)((uintptr_t)buf + off), chunk);
        written += chunk;
        remaining -= chunk;

        if (bi + 1 < num_buffers) {
            if (!(desc[di].flags & VIRTQ_DESC_F_NEXT)) break;
            di = desc[di].next;
        }
    }

    disable_interrupt();
    uint16_t aidx = avail->idx;
    avail->ring[aidx % qsz] = (uint16_t)desc_index;
    asm volatile ("dmb ishst" ::: "memory");
    avail->idx = (uint16_t)(aidx + 1);
    asm volatile ("dmb ishst" ::: "memory");
    virtio_notify(&vnp_net_dev);
    enable_interrupt();

    if (remaining != 0) {
        kfree(out_buf, payload_len);
        return (sizedptr){0,0};
    }

    return (sizedptr){ (uintptr_t)out_buf, payload_len };
}

void VirtioNetDriver::handle_sent_packet(){
    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);

    volatile virtq_used* used = (volatile virtq_used*)PHYS_TO_VIRT_P((void*)(uintptr_t)vnp_net_dev.common_cfg->queue_device);
    last_used_sent_idx = used->idx;
}

bool VirtioNetDriver::send_packet(sizedptr packet){
    if (!packet.ptr || !packet.size) return false;

    disable_interrupt();
    select_queue(&vnp_net_dev, TRANSMIT_QUEUE);

    if ((size_t)header_size <= packet.size) memset((void*)packet.ptr, 0, (size_t)header_size);
    if (mrg_rxbuf) ((virtio_net_hdr_mrg_rxbuf_t*)packet.ptr)->num_buffers = 0;
    virtio_buf b;
    b.addr = packet.ptr;
    b.len = (uint32_t)packet.size;
    b.flags = 0;
    bool ok = virtio_send_nd(&vnp_net_dev, &b, 1);
    enable_interrupt();

    kprintfv("[virtio-net] tx queued len=%u",(unsigned)packet.size);
    kfree((void*)packet.ptr, packet.size);
    return ok;
}

bool VirtioNetDriver::sync_multicast(const uint8_t* macs, uint32_t count) {
    if (!ctrl_vq) return true;
    if (!ctrl_rx) return true;
    if (!macs && count) return false;

    disable_interrupt();

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
        enable_interrupt();
        return false;
    }
    kprintfv("[virtio-net] sync_multicast ctrl_vq=%u ctrl_rx=%u count=%u",(unsigned)ctrl_vq, (unsigned)ctrl_rx, (unsigned)count);

    uint32_t uc = 0;
    memcpy(payload + 0, &uc, 4);
    memcpy(payload + 4, &count, 4);
    for (uint32_t i = 0; i < count; ++i) memcpy(payload + 8u + i * 6u, macs + i * 6u, 6);

    ok = ok && virtio_net_ctrl_send(&vnp_net_dev, VIRTIO_NET_CTRL_MAC, VIRTIO_NET_CTRL_MAC_TABLE_SET, payload, payload_len);

    kfree(payload, payload_len);
    enable_interrupt();
    return ok;
}

void VirtioNetDriver::enable_verbose(){
    verbose = true;
}
#include "network_dispatch.hpp"
#include "drivers/virtio_net_pci/virtio_net_pci.hpp"
#include "drivers/net_bus.hpp"
#include "memory/page_allocator.h"
#include "net/link_layer/eth.h"
#include "net/network_types.h"
#include "port_manager.h"
#include "std/memory.h"
#include "std/std.h"
#include "console/kio.h"
#include "networking/interface_manager.h"

extern void      sleep(uint64_t ms);
extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);

#define RX_INTR_BATCH_LIMIT 32
#define TASK_RX_BATCH_LIMIT 32
#define TASK_TX_BATCH_LIMIT 32

NetworkDispatch::NetworkDispatch()
{
    nic_num = 0;
    g_net_pid = 0xFFFF;
    for (int i = 0; i <= (int)MAX_L2_INTERFACES; ++i) ifindex_to_nicid[i] = 0xFF;
}

bool NetworkDispatch::init()
{
    if (net_bus_init() <= 0) return false;
    if (!register_all_from_bus()) return false;

    l3_init_localhost_ipv4();
    l3_init_localhost_ipv6();
    ifmgr_autoconfig_all_l2();

    dump_interfaces();

    for (int ix = 1; ix <= (int)MAX_L2_INTERFACES; ++ix){
        int nid = nic_for_ifindex((uint8_t)ix);
        if (nid >= 0) {
            const char* nm = nics[nid].ifname_str;
            kprintf("[net] ifindex=%i -> nic_id=%i (%s)", ix, nid, nm );
        }
    }
    return nic_num > 0;
}

void NetworkDispatch::handle_rx_irq(size_t nic_id)
{
    if (nic_id >= nic_num) return;
    NetDriver* driver = nics[nic_id].drv;
    if (!driver) return;
    for (int i = 0; i < RX_INTR_BATCH_LIMIT; ++i) {
        sizedptr raw = driver->handle_receive_packet();
        if (!raw.ptr || raw.size == 0) break;
        if (raw.size < sizeof(eth_hdr_t)) { free_frame(raw); continue; }
        if (!nics[nic_id].rx.enqueue(raw)) free_frame(raw);
    }
}

void NetworkDispatch::handle_tx_irq(size_t nic_id)
{
    if (nic_id >= nic_num) return;
    NetDriver* driver = nics[nic_id].drv;
    if (!driver) return;
    driver->handle_sent_packet();
}

bool NetworkDispatch::enqueue_frame(uint8_t ifindex, const sizedptr& frame)
{
    int nic_id = nic_for_ifindex(ifindex);
    if (nic_id < 0) return false;
    NetDriver* driver = nics[nic_id].drv;
    if (!driver) return false;
    if (frame.size == 0) return false;

    sizedptr pkt = driver->allocate_packet(frame.size);
    if (!pkt.ptr) return false;

    uint16_t hs = nics[nic_id].hdr_sz;
    void* dst = (void*)(pkt.ptr + hs);
    memcpy(dst, (const void*)frame.ptr, frame.size);

    if (!nics[nic_id].tx.enqueue(pkt)) { free_frame(pkt); return false; }
    return true;
}

int NetworkDispatch::net_task()
{
    set_net_pid(get_current_proc_pid());
    for (;;) {
        bool did_work = false;

        for (size_t n = 0; n < nic_num; ++n) {
            for (int i = 0; i < TASK_RX_BATCH_LIMIT; ++i) {
                if (nics[n].rx.is_empty()) break;
                sizedptr pkt{0,0};
                if (!nics[n].rx.dequeue(pkt)) break;
                did_work = true;
                eth_input(pkt.ptr, pkt.size);
                free_frame(pkt);
            }
        }

        for (size_t n = 0; n < nic_num; ++n) {
            NetDriver* driver = nics[n].drv;
            if (!driver) continue;
            for (int i = 0; i < TASK_TX_BATCH_LIMIT; ++i) {
                if (nics[n].tx.is_empty()) break;
                sizedptr pkt{0,0};
                if (!nics[n].tx.dequeue(pkt)) break;
                did_work = true;
                driver->send_packet(pkt);
            }
        }

        if (!did_work) sleep(10);//TODO: manage it with an event
    }
}

void NetworkDispatch::set_net_pid(uint16_t pid)
{
    g_net_pid = pid;
}

uint16_t NetworkDispatch::get_net_pid() const
{
    return g_net_pid;
}

size_t NetworkDispatch::nic_count() const
{
    return nic_num;
}

const char* NetworkDispatch::ifname(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? nullptr : nics[nic_id].ifname_str;
}

const char* NetworkDispatch::hw_ifname(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? nullptr : nics[nic_id].hwname_str;
}

const uint8_t* NetworkDispatch::mac(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? nullptr : nics[nic_id].mac_addr;
}

uint16_t NetworkDispatch::mtu(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? 0 : nics[nic_id].mtu_val;
}

uint16_t NetworkDispatch::header_size(uint8_t ifindex) const 
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? 0 : nics[nic_id].hdr_sz;
}

l2_interface_t* NetworkDispatch::l2_at(uint8_t ifindex) const
{
    return l2_interface_find_by_index(ifindex);
}

NetDriver* NetworkDispatch::driver_at(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? nullptr : nics[nic_id].drv;
}

void NetworkDispatch::free_frame(const sizedptr &f)
{
    if (f.ptr) free_sized(f);
}

bool NetworkDispatch::register_all_from_bus() {
    int n = net_bus_count();
    if (n <= 0) return false;

    for (int i = 0; i < n && nic_num < MAX_NIC; ++i) {
        NetDriver* drv = net_bus_driver(i);
        const char* name = net_bus_ifname(i);
        const char* hw = net_bus_hw_ifname(i);
        uint16_t m = net_bus_get_mtu(i);
        uint16_t hs = net_bus_get_header_size(i);
        uint8_t macbuf[6]; net_bus_get_mac(i, macbuf);

        if (!name) continue;

        if (!drv) {
            bool is_lo0 = (name[0]=='l' && name[1]=='o' && name[2]=='0' && name[3]==0);
            if (!is_lo0) continue;

            uint8_t ix = l2_interface_create(name, nullptr);
            l2_interface_set_up(ix, true);
            continue;
        }

        NICCtx* c = &nics[nic_num];
        c->drv = drv;

        new ((void*)&c->tx) Queue<sizedptr>(QUEUE_CAPACITY);
        new ((void*)&c->rx) Queue<sizedptr>(QUEUE_CAPACITY);

        copy_str(c->ifname_str, (int)sizeof(c->ifname_str), name);
        copy_str(c->hwname_str, (int)sizeof(c->hwname_str), hw);
        memcpy(c->mac_addr, macbuf, 6);
        c->mtu_val = m;
        c->hdr_sz = hs;

        uint8_t ix = l2_interface_create(c->ifname_str, (void*)drv);
        l2_interface_set_up(ix, true);
        c->ifindex = ix;

        if (ix <= MAX_L2_INTERFACES) ifindex_to_nicid[ix] = (uint8_t)nic_num;

        nic_num += 1;
    }
    return nic_num > 0;
}

void NetworkDispatch::copy_str(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    if (!src) { dst[0] = 0; return; }
    uint32_t n = strlen(src, (uint32_t)(cap - 1));
    memcpy(dst, src, n);
    dst[n] = 0;
}

int NetworkDispatch::nic_for_ifindex(uint8_t ifindex) const {
    if (!ifindex) return -1;
    if (ifindex > MAX_L2_INTERFACES) return -1;
    uint8_t nic_id = ifindex_to_nicid[ifindex];
    if (nic_id == 0xFF) return -1;
    if (nic_id >= nic_num) return -1;
    return (int)nic_id;
}

void NetworkDispatch::dump_interfaces()
{
    kprintf("[net]interface dump start");

    auto ipv6_to_str = [&](const uint8_t ip[16], char out[41]){ //TODO: move this to ipv6 file
        static const char HEX[] = "0123456789abcdef";
        int p = 0;
        for (int g = 0; g < 8; ++g) {
            uint16_t w = (uint16_t(ip[g*2]) << 8) | uint16_t(ip[g*2 + 1]);
            out[p++] = HEX[(w >> 12) & 0xF];
            out[p++] = HEX[(w >> 8) & 0xF];
            out[p++] = HEX[(w >> 4) & 0xF];
            out[p++] = HEX[w & 0xF];
            if (g != 7) out[p++] = ':';
        }
        out[p] = 0;
    };

    for (uint8_t ifx = 1; ifx <= (uint8_t)MAX_L2_INTERFACES; ++ifx){
        l2_interface_t* l2 = l2_interface_find_by_index(ifx);
        if (!l2) continue;

        int nid = nic_for_ifindex(ifx);
        kprintf("int l2 %u: name=%s up=%u ipv4_count=%u ipv6_count=%u arp=%x nd=%x mcast4=%u mcast6=%u",
                (unsigned)ifx, l2->name, l2->is_up?1:0,
                (unsigned)l2->ipv4_count, (unsigned)l2->ipv6_count,
                (uint64_t)(uintptr_t)l2->arp_table, (uint64_t)(uintptr_t)l2->nd_table,
                (unsigned)l2->ipv4_mcast_count, (unsigned)l2->ipv6_mcast_count);

        if (nid >= 0){
            char macs[18];
            {
                static const char HEX[] = "0123456789abcdef";
                int p = 0;
                for (int i = 0; i < 6; ++i) {
                    uint8_t b = nics[nid].mac_addr[i];
                    macs[p++] = HEX[b >> 4];
                    macs[p++] = HEX[b & 0x0F];
                    if (i != 5) macs[p++] = ':';
                }
                macs[p] = 0;
            }

            kprintf(" driver: nic_id=%u ifname=%s hw=%s mtu=%u hdr=%u mac=%s drv=%x",
                    (unsigned)nid,
                    nics[nid].ifname_str[0] ? nics[nid].ifname_str : "(null)",
                    nics[nid].hwname_str[0] ? nics[nid].hwname_str : "(null)",
                    (unsigned)nics[nid].mtu_val, (unsigned)nics[nid].hdr_sz, macs,
                    (uint64_t)(uintptr_t)nics[nid].drv);
        } else {
            kprintf(" driver: none");
        }

        kprintf(" int ipv4:");
        for (int s = 0; s < (int)MAX_IPV4_PER_INTERFACE; ++s){
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4) continue;
            char ip[16], mask[16], gw[16], bc[16];
            ipv4_to_string(v4->ip, ip);
            ipv4_to_string(v4->mask, mask);
            ipv4_to_string(v4->gw, gw);
            ipv4_to_string(v4->broadcast, bc);
            kprintf("  - slot=%u l3_id=%u mode=%i ip=%s mask=%s gw=%s bcast=%s rt=%x localhost=%u",
                    (unsigned)s, (unsigned)v4->l3_id, (int)v4->mode, ip, mask, gw, bc,
                    (uint64_t)(uintptr_t)v4->rt_v4, v4->is_localhost?1:0);
        }

        kprintf(" int ipv6:");
        for (int s = 0; s < (int)MAX_IPV6_PER_INTERFACE; ++s){
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!v6) continue;
            char ip6[41], gw6[41];
            ipv6_to_str(v6->ip, ip6);
            ipv6_to_str(v6->gateway, gw6);
            uint32_t llc = (v6->kind & IPV6_ADDRK_LINK_LOCAL) ? 1u : 0u;
            uint32_t gua = (v6->kind & IPV6_ADDRK_GLOBAL) ? 1u : 0u;
            uint32_t en = (v6->cfg != IPV6_CFG_DISABLE) ? 1u : 0u;
            kprintf("  - slot=%u l3_id=%u kind=%u cfg=%i llc=%u gua=%u en=%u ip=%s/%u gw=%s vlft=%u plft=%u tsc=%u localhost=%u",
                    (unsigned)s, (unsigned)v6->l3_id, (unsigned)v6->kind, (int)v6->cfg, llc, gua, en,
                    ip6, (unsigned)v6->prefix_len, gw6,
                    (unsigned)v6->valid_lifetime, (unsigned)v6->preferred_lifetime, (unsigned)v6->timestamp_created,
                    v6->is_localhost?1:0);
        }
    }

    kprintf("[net]interface dump end");
}
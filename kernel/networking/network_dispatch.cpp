#include "network_dispatch.hpp"
#include "drivers/virtio_net_pci/virtio_net_pci.hpp"
#include "drivers/net_bus.hpp"
#include "memory/page_allocator.h"
#include "networking/link_layer/eth.h"
#include "net/network_types.h"
#include "port_manager.h"
#include "std/memory.h"
#include "std/std.h"
#include "console/kio.h"
#include "networking/interface_manager.h"
#include "process/scheduler.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/netpkt.h"
#include "networking/link_layer/link_utils.h"
#include "networking/drivers/loopback/loopback_driver.hpp"

#define RX_INTR_BATCH_LIMIT 64
#define TASK_RX_BATCH_LIMIT 256
#define TASK_TX_BATCH_LIMIT 256

NetworkDispatch::NetworkDispatch()
{
    nic_num = 0;
    g_net_pid = 0xFFFF;
    for (int i = 0; i <= (int)MAX_L2_INTERFACES; ++i) ifindex_to_nicid[i] = 0xFF;
    for (size_t i = 0; i < MAX_NIC; ++i) {
        nics[i].drv = nullptr;
        nics[i].ifindex = 0;
        nics[i].ifname_str[0] = 0;
        nics[i].hwname_str[0] = 0;
        nics[i].mtu_val = 0;
        nics[i].hdr_sz = 0;
        nics[i].speed_mbps = 0xFFFFFFFFu;
        nics[i].duplex_mode = 0xFFu;
        nics[i].kind_val = 0xFFu;
        nics[i].rx_produced = 0;
        nics[i].rx_consumed = 0;
        nics[i].tx_produced = 0;
        nics[i].tx_consumed = 0;
        nics[i].rx_dropped = 0;
        nics[i].tx_dropped = 0;
    }
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
    if (!nics[nic_id].drv) return;
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

    if (!nics[nic_id].tx.push(pkt)) {
        free_frame(pkt);
        nics[nic_id].tx_dropped++;
        return false;
    }
    nics[nic_id].tx_produced++;
    return true;
}

int NetworkDispatch::net_task()
{
    set_net_pid(get_current_proc_pid());
    for (;;) {
        bool did_work = false;

        for (size_t n = 0; n < nic_num; ++n) {
            NetDriver* driver = nics[n].drv;
            if (driver) {
                int lim = nics[n].kind_val == NET_IFK_LOCALHOST ? TASK_RX_BATCH_LIMIT : RX_INTR_BATCH_LIMIT;
                for (int i = 0; i < lim; ++i) {
                    sizedptr raw = driver->handle_receive_packet();
                    if (!raw.ptr || raw.size == 0) break;
                    if (raw.size < sizeof(eth_hdr_t)) {
                        free_frame(raw);
                        continue;
                    }
                    if (!nics[n].rx.push(raw)) {
                        free_frame(raw);
                        nics[n].rx_dropped++;
                        continue;
                    }
                    nics[n].rx_produced++;
                }
            }
            int processed = 0;
            for (int i = 0; i < TASK_RX_BATCH_LIMIT; ++i) {
                if (nics[n].rx.is_empty()) break;
                sizedptr pkt{0,0};
                if (!nics[n].rx.pop(pkt)) break;
                netpkt_t* np = netpkt_wrap(pkt.ptr, pkt.size, pkt.size, NULL, 0);
                if (np) {
                    eth_input(nics[n].ifindex, np);
                    netpkt_unref(np);
                } else {
                    free_frame(pkt);
                }
                nics[n].rx_consumed++;
                processed++;
            }
            if (processed) did_work = true;
        }

        for (size_t n = 0; n < nic_num; ++n) {
            NetDriver* driver = nics[n].drv;
            if (!driver) continue;
            int processed = 0;
            for (int i = 0; i < TASK_TX_BATCH_LIMIT; ++i) {
                if (nics[n].tx.is_empty()) break;
                sizedptr pkt{0,0};
                if (!nics[n].tx.pop(pkt)) break;
                if (!driver->send_packet(pkt)) {
                    free_frame(pkt);
                    nics[n].tx_dropped++;
                }
                nics[n].tx_consumed++;
                processed++;
            }
            if (processed) did_work = true;
        }

        if (!did_work) msleep(0);//TODO: manage it with an event
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

uint32_t NetworkDispatch::speed(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? 0xFFFFFFFFu : nics[nic_id].speed_mbps;
}

uint8_t NetworkDispatch::duplex(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? 0xFFu : nics[nic_id].duplex_mode;
}

uint8_t NetworkDispatch::kind(uint8_t ifindex) const
{
    int nic_id = nic_for_ifindex(ifindex);
    return nic_id < 0 ? 0xFFu : nics[nic_id].kind_val;
}

void NetworkDispatch::free_frame(const sizedptr &f)
{
    if (f.ptr) free_sized((void*)f.ptr, f.size);
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
        uint32_t sp = net_bus_get_speed_mbps(i);
        uint8_t dp = net_bus_get_duplex(i);
        uint8_t kd = net_bus_get_kind(i);
        uint8_t macbuf[6]; net_bus_get_mac(i, macbuf);

        if (!name) continue;

        if (!drv) {
            if (kd != NET_IFK_LOCALHOST) continue;
			LoopbackDriver* lo_drv = new LoopbackDriver();
			if (!lo_drv) continue;
			if (!lo_drv->init_at(0, 0)) {
                delete lo_drv;
                continue;
            }
			drv = lo_drv;
            if (!hs) hs = 0;
            if (!m) m = 65535;
        }

        uint16_t type_cost =
            (kd == NET_IFK_LOCALHOST) ? 0 :
            (kd == NET_IFK_ETH) ? 20 :
            (kd == NET_IFK_WIFI) ? 40 : 30;

        uint16_t speed_cost =
            (sp == 0xFFFFFFFFu) ? 10 :
            (sp >= 40000u) ? 0 :
            (sp >= 10000u) ? 2 :
            (sp >= 1000u) ? 5 :
            (sp >= 100u) ? 15 : 40;

        uint16_t duplex_cost =
            (dp == 1) ? 0 :
            (dp == 0) ? 20 : 5;

        uint16_t mtu_cost =
            (m >= 9000u) ? 0 :
            (m >= 2000u) ? 3 :
            (m >= 1500u) ? 5 :
            (m >= 576u) ? 15 : 100;

        uint16_t base_metric = (kd == NET_IFK_LOCALHOST) ? 0 : (uint16_t)(type_cost + speed_cost + duplex_cost + mtu_cost);

        NICCtx* c = &nics[nic_num];
        c->drv = drv;

        strncpy(c->ifname_str, name, (int)sizeof(c->ifname_str));
        strncpy(c->hwname_str, hw, (int)sizeof(c->hwname_str));
        memcpy(c->mac_addr, macbuf, 6);
        c->mtu_val = m;
        c->hdr_sz = hs;
        c->speed_mbps = sp;
        c->duplex_mode = dp;
        c->kind_val = kd;

        uint8_t ix = l2_interface_create(c->ifname_str, (void*)drv, base_metric, kd);
        l2_interface_set_up(ix, true);
        c->ifindex = ix;

        if (ix <= MAX_L2_INTERFACES) ifindex_to_nicid[ix] = (uint8_t)nic_num;

        nic_num += 1;
    }
    return nic_num > 0;
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
            mac_to_string(nics[nid].mac_addr, macs);

            const char* dpx = (nics[nid].duplex_mode == 0) ? "half" : (nics[nid].duplex_mode == 1) ? "full" : "unknown";

            kprintf(" driver: nic_id=%u ifname=%s hw=%s mtu=%u hdr=%u mac=%s drv=%x spd=%u dup=%s kind=%u",
                    (unsigned)nid,
                    nics[nid].ifname_str[0] ? nics[nid].ifname_str : "(null)",
                    nics[nid].hwname_str[0] ? nics[nid].hwname_str : "(null)",
                    (unsigned)nics[nid].mtu_val, (unsigned)nics[nid].hdr_sz, macs,
                    (uint64_t)(uintptr_t)nics[nid].drv,
                    (unsigned)nics[nid].speed_mbps, dpx, (unsigned)nics[nid].kind_val);
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

            char dns0[16], dns1[16], ntp0[16], ntp1[16];
            if (v4->runtime_opts_v4.dns[0]) ipv4_to_string(v4->runtime_opts_v4.dns[0], dns0); else { dns0[0]='-'; dns0[1]=0; }
            if (v4->runtime_opts_v4.dns[1]) ipv4_to_string(v4->runtime_opts_v4.dns[1], dns1); else { dns1[0]='-'; dns1[1]=0; }
            if (v4->runtime_opts_v4.ntp[0]) ipv4_to_string(v4->runtime_opts_v4.ntp[0], ntp0); else { ntp0[0]='-'; ntp0[1]=0; }
            if (v4->runtime_opts_v4.ntp[1]) ipv4_to_string(v4->runtime_opts_v4.ntp[1], ntp1); else { ntp1[0]='-'; ntp1[1]=0; }

            kprintf("  - slot=%u l3_id=%u mode=%i ip=%s mask=%s gw=%s bcast=%s "
                    "mtu=%u dns=[%s,%s] ntp=[%s,%s] xid=%u lease=%us t1=%us t2=%us localhost=%u",
                    (unsigned)s, (unsigned)v4->l3_id, (int)v4->mode,
                    ip, mask, gw, bc,
                    (unsigned)v4->runtime_opts_v4.mtu,
                    dns0, dns1,
                    ntp0, ntp1,
                    (unsigned)v4->runtime_opts_v4.xid,
                    (unsigned)v4->runtime_opts_v4.lease,
                    (unsigned)v4->runtime_opts_v4.t1,
                    (unsigned)v4->runtime_opts_v4.t2,
                    v4->is_localhost ? 1u : 0u);
        }


        kprintf(" int ipv6:");
        for (int s = 0; s < (int)MAX_IPV6_PER_INTERFACE; ++s){
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!v6) continue;

            char ip6[41], gw6[41];
            ipv6_to_string(v6->ip, ip6, (int)sizeof(ip6));
            ipv6_to_string(v6->gateway, gw6, (int)sizeof(gw6));

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
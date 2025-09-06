#include "network_dispatch.hpp"
#include "drivers/virtio_net_pci/virtio_net_pci.hpp"
#include "drivers/net_bus.hpp"
#include "memory/page_allocator.h"
#include "net/link_layer/eth.h"
#include "net/network_types.h"
#include "port_manager.h"
#include "std/memory.h"
#include "std/std.h"

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
}

bool NetworkDispatch::init()
{
    if (net_bus_init() <= 0) return false;
    if (!register_all_from_bus()) return false;
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

bool NetworkDispatch::enqueue_frame_on(size_t nic_id, const sizedptr& frame)
{
    if (nic_id >= nic_num) return false;
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

const char* NetworkDispatch::ifname(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0;
    return nics[nic_id].ifname_str;
}

const char* NetworkDispatch::hw_ifname(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0;
    return nics[nic_id].hwname_str;
}

const uint8_t* NetworkDispatch::mac(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0;
    return nics[nic_id].mac_addr;
}

uint16_t NetworkDispatch::mtu(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0;
    return nics[nic_id].mtu_val;
}

uint16_t NetworkDispatch::header_size(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0;
    return nics[nic_id].hdr_sz;
}

uint8_t NetworkDispatch::ifindex(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0xFF;
    return nics[nic_id].ifindex;
}

l2_interface_t* NetworkDispatch::l2_at(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0;
    return l2_interface_find_by_index(nics[nic_id].ifindex);
}

NetDriver* NetworkDispatch::driver_at(size_t nic_id) const
{
    if (nic_id >= nic_num) return 0;
    return nics[nic_id].drv;
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
            uint8_t zero_mac[6] = {0,0,0,0,0,0};
            l2_interface_set_mac(ix, zero_mac);
            l2_interface_set_mtu(ix, m ? m : 65535);
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
        l2_interface_set_mac(ix, c->mac_addr);
        l2_interface_set_mtu(ix, c->mtu_val);
        l2_interface_set_up(ix, true);
        c->ifindex = ix;

        nic_num += 1;
    }
    return nic_num > 0;
}


void NetworkDispatch::copy_str(char* dst, int cap, const char* src)
{
    if (!dst || cap <= 0) return;
    if (!src) { dst[0] = 0; return; }
    uint32_t n = strlen(src, (uint32_t)(cap - 1));
    memcpy(dst, src, n);
    dst[n] = 0;
}

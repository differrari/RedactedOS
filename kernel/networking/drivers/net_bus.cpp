#include "net_bus.hpp"
#include "net_driver.hpp"
#include "virtio_net_pci/virtio_net_pci.hpp"
#include "pci.h"
#include "std/std.h"
#include "console/kio.h"
#include "types.h"
#include "networking/interface_manager.h"
#include "networking/network.h"

#define kprintfv(fmt, ...) ({ if (verbose){ kprintf(fmt, ##__VA_ARGS__); } })

typedef struct {
    NetDriver* drv;
    char ifname[16];
    char hw_ifname[32];
    uint8_t mac[6];
    uint16_t mtu;
    uint16_t header_size;
    uint8_t kind;
    uint32_t speed_mbps;
    uint8_t duplex;
} net_nic_desc_t;

static net_nic_desc_t g_nics[MAX_L2_INTERFACES];
static size_t g_count = 0;
static int g_eth_next = 0;
static int g_wif_next = 0;
static int g_net_next = 0;
static bool g_lo_added = false;
static bool verbose = true;

static void make_ifname(char* dst, size_t cap, const char* prefix){
    if (!dst || cap == 0) return;

    int idx = 0;
    const char* pfx = prefix;

    if (pfx && pfx[0] == 'e') idx = g_eth_next++;
    else if (pfx && pfx[0] == 'w') idx = g_wif_next++;
    else idx = g_net_next++;

    if (!pfx || !pfx[0]) pfx = "nic";

    string_format_buf(dst, cap, "%s%u", pfx, (unsigned)idx);
}

static void add_loopback(){
    if (g_lo_added) return;
    if (g_count >= MAX_L2_INTERFACES) return;
    net_nic_desc_t* d = &g_nics[g_count++];
    d->drv = nullptr;

    memset(d->ifname, 0, sizeof(d->ifname));
    memset(d->hw_ifname, 0, sizeof(d->hw_ifname));
    memset(d->mac, 0, sizeof(d->mac));

    strncpy(d->ifname, "lo0", sizeof(d->ifname));
    strncpy(d->hw_ifname, "loopback", sizeof(d->hw_ifname));

    d->mtu = 65535;
    d->header_size = 0;
    d->kind = NET_IFK_LOCALHOST;
    d->speed_mbps = 0xFFFFFFFFu;
    d->duplex = LINK_DUPLEX_UNKNOWN;
    g_lo_added = true;
    kprintfv("[net-bus] added loopback ifname=%s",d->ifname);
}

static bool is_virtio_net(uint16_t vendor, uint16_t device, uint8_t class_code, uint8_t subclass) {
    if (vendor != 0x1AF4) return false;
    if (class_code == 0x02) return true;
    if (device == 0x1000) return true;
    (void)subclass;
    return false;
}

int net_bus_init(){
    g_count = 0;
    g_eth_next = 0;
    g_wif_next = 0;
    g_net_next = 0;
    g_lo_added = false;

    kprintfv("[net-bus] init");

    pci_device_info infos[64];
    size_t n = pci_enumerate(infos, 64);
    kprintfv("[net-bus] pci_enumerate=%u", (unsigned)n);

    int nic_ord = 0;

    for (size_t i = 0; i < n; i++){
        if (g_count >= MAX_L2_INTERFACES) {
            kprintfv("[net-bus] cap reached");
            break;
        }

        const uint16_t ven = infos[i].vendor;
        const uint16_t dev = infos[i].device;
        const uint8_t cls = infos[i].class_code;
        const uint8_t sub = infos[i].subclass;

        if (cls != 0x02) continue;

        kprintfv("[net-bus] net dev %u ven=%x dev=%x class=%x sub=%x prog=%x addr=%x",
                 (unsigned)i, ven, dev, cls, sub, infos[i].prog_if, (uintptr_t)infos[i].addr);

        const char* if_prefix = "net";
        uint8_t kind = NET_IFK_OTHER;
        if (sub == 0x00) {
            if_prefix = "eth";
            kind = NET_IFK_ETH;
        }

        bool matched = false;

        if (is_virtio_net(ven, dev, cls, sub)){
            VirtioNetDriver* d = new VirtioNetDriver();
            if (!d) {
                kprintf("[net-bus][warn] virtio alloc failed");
                continue;
            }

            uint32_t irq_base = NET_IRQ_BASE + (uint32_t)(2 * nic_ord);
            if (!d->init_at(infos[i].addr, irq_base)) {
                kprintf("[net-bus][warn] virtio init_at failed");
                delete d;
                continue;
            }

            net_nic_desc_t* e = &g_nics[g_count++];
            memset(e, 0, sizeof(*e));

            e->drv = d;

            d->get_mac(e->mac);
            e->mtu = d->get_mtu();
            e->header_size = d->get_header_size();
            e->kind = kind;
            e->speed_mbps = d->get_speed_mbps();
            e->duplex = d->get_duplex();

            make_ifname(e->ifname, sizeof(e->ifname), if_prefix);

            const char* hw = d->hw_ifname();
            strncpy(e->hw_ifname, (hw && hw[0]) ? hw : "vnet", sizeof(e->hw_ifname));

            kprintfv("[net-bus] added if=%s mac=%x:%x:%x:%x:%x:%x mtu=%u hdr=%u hw=%s irq_base=%u spd=%u dup=%u",
                     e->ifname, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                     e->mtu, e->header_size, e->hw_ifname, (unsigned)irq_base,
                     (unsigned)e->speed_mbps, (unsigned)e->duplex);

            nic_ord += 1;
            matched = true;
        }
        //else if future drivers

        if (!matched){
            kprintf("[net-bus][warn] no driver for ven=%x dev=%x class=%x sub=%x", ven, dev, cls, sub);
        }
    }

    if (!g_lo_added && g_count < MAX_L2_INTERFACES) add_loopback();

    kprintfv("[net-bus] total_if=%u",(unsigned)g_count);
    return (int)g_count;
}

void net_bus_enable_verbose(){
    verbose = true;
}

int net_bus_count(){
    return (int)g_count;
}

NetDriver* net_bus_driver(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return nullptr;
    return g_nics[idx].drv;
}

const char* net_bus_ifname(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return nullptr;
    return g_nics[idx].ifname;
}

const char* net_bus_hw_ifname(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return nullptr;
    return g_nics[idx].hw_ifname;
}

void net_bus_get_mac(int idx, uint8_t out_mac[6]) {
    if (!out_mac) return;
    if (idx < 0 || (size_t)idx >= g_count) {
        memset(out_mac, 0, 6);
        return;
    }
    memcpy(out_mac, g_nics[idx].mac, 6);
}

uint16_t net_bus_get_mtu(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return 0;
    return g_nics[idx].mtu;
}

uint16_t net_bus_get_header_size(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return 0;
    return g_nics[idx].header_size;
}

uint8_t net_bus_get_kind(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return NET_IFK_UNKNOWN;
    return g_nics[idx].kind;
}

uint32_t net_bus_get_speed_mbps(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return 0xFFFFFFFFu;
    return g_nics[idx].speed_mbps;
}

uint8_t net_bus_get_duplex(int idx){
    if (idx < 0 || (size_t)idx >= g_count) return 0xFFu;
    return g_nics[idx].duplex;
}

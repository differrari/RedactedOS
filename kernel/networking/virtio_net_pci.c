#include "virtio_net_pci.h"
#include "std/string.h"
#include "memory/memory_access.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "pci.h"
#include "virtio/virtio_pci.h"
#include "std/memfunctions.h"

static virtio_device vnp_net_dev;

bool vnp_find_network(){
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

    uint8_t* mac = vnp_net_dev.device_cfg;
    kprintf("%x:%x:%x:%x:%x:%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return true;
}
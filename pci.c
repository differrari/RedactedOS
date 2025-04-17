#include "pci.h"
#include "uart.h"
#include "mmio.h"

#define PCI_ECAM_BASE 0x4010000000
#define PCI_BUS_MAX 256
#define PCI_SLOT_MAX 32
#define PCI_FUNC_MAX 8

#define PCI_COMMAND_REGISTER 0x04

uint64_t pci_make_addr(uint32_t bus, uint32_t slot, uint32_t func, uint32_t offset){
    return PCI_ECAM_BASE
            | (((uint64_t)bus) << 20)
            | (((uint64_t)slot) << 15)
            | (((uint64_t)func) << 12) 
            | ((uint64_t)(offset & 0xFFF));
}

void inspect_bar(uint64_t base) {
    uart_puts("Inspecting GPU BARs...\n");
    for (uint32_t bar_offset = 0x10; bar_offset <= 0x28; bar_offset += 4) {
        uint64_t bar = mmio_read(base + bar_offset);
        uart_puts("BAR @ offset ");
        uart_puthex(bar_offset);
        uart_puts(": ");
        uart_puthex(bar);
        uart_putc('\n');

        // Check for 64-bit BAR
        if ((bar & 0x4) != 0) {
            uart_puts("  - 64-bit Prefetchable Memory\n");
            uint64_t high = mmio_read(base + bar_offset + 4);
            uart_puts("  - Upper 32 bits: ");
            uart_puthex(high);
            uart_putc('\n');
            bar |= ((uint64_t)high << 32);
        }

        // Check if MMIO or IO
        if (bar & 1) {
            uart_puts("  - I/O Space\n");
        } else {
            uint64_t mmio_base = bar & ~0xF;
            uart_puts("  - MMIO Space\n");
            uart_puts("  - MMIO Base: ");
            uart_puthex(mmio_base);
            uart_putc('\n');
        }
    }
}

uint64_t find_pci_device(uint32_t vendor_id, uint32_t device_id, uint64_t* out_mmio_base) {
    for (uint32_t bus = 0; bus < PCI_BUS_MAX; bus++) {
        for (uint32_t slot = 0; slot < PCI_SLOT_MAX; slot++) {
            for (uint32_t func = 0; func < PCI_FUNC_MAX; func++) {
                uint64_t device_address = pci_make_addr(bus, slot, func, 0x00);
                uint64_t vendor_device = mmio_read(device_address);
                if ((vendor_device & 0xFFFF) == vendor_id && ((vendor_device >> 16) & 0xFFFF) == device_id) {

                    uart_puts("Found device at bus ");
                    uart_puthex(bus);
                    uart_puts(", slot ");
                    uart_puthex(slot);
                    uart_puts(", func ");
                    uart_puthex(func);
                    uart_putc('\n');

                    // inspect_bar(device_address);

                    *out_mmio_base = device_address;

                    return device_address;

                    // for (uint32_t bar_offset = 0x10; bar_offset <= 0x24; bar_offset += 4) {
                    //     uint64_t baraddr = pci_make_addr(bus,slot,func,bar_offset);
                    //     uint64_t bar = mmio_read(baraddr);
                    //     if (bar == 0 || bar == 0xFFFFFFFF) continue;

                    //     if ((bar & 1) == 0) { 
                    //         *out_mmio_base = bar & ~0xF;
                    //         return device_address;
                    //     }
                    // }
                }
            }
        }
    }
    uart_puts("Device not found.\n");
    return 0;
}

void dump_pci_config(uint64_t base) {
    uart_puts("Dumping PCI Configuration Space:\n");
    for (uint32_t offset = 0; offset < 0x40; offset += 4) {
        uint64_t val = mmio_read(base + offset);
        uart_puts("Offset ");
        uart_puthex(offset);
        uart_puts(": ");
        uart_puthex(val);
        uart_putc('\n');
    }
}

void pci_enable_device(uint64_t base) {
    uint64_t cmd_before = mmio_read(base + 0x04);
    uart_puts("PCI Command Register before: ");
    uart_puthex(cmd_before);
    uart_putc('\n');

    // Set the Memory Space Enable (MSE) and Bus Master Enable (BME) bits
    uint64_t cmd = cmd_before | 0x7;

    uart_puts("Setting CMD: ");
    uart_puthex(cmd);
    uart_putc('\n');

    mmio_write(base + 0x04, cmd);

    uint64_t cmd_after = mmio_read(base + 0x04);
    uart_puts("PCI Command Register after: ");
    uart_puthex(cmd_after);
    uart_putc('\n');

    if ((cmd_after & 0x7) == 0x7) {
        uart_puts("PCI device successfully enabled.\n");
    } else {
        uart_puts("Failed to enable PCI device (MSE/BME not set).\n");
    }
}

//Network device
// if (find_pci_device(0x1AF4, 0x1000, &bus, &slot, &func, &mmio_base)) {
//     uart_puts("Virtio Network device detected with MMIO base: ");
//     uart_puthex(mmio_base);
//     uart_putc('\n');
// }
#include "pci.h"
#include "console/kio.h"
#include "exceptions/exception_handler.h"
#include "memory/talloc.h"
#include "memory/mmu.h"
#include "memory/memory_access.h"
#include "hw/hw.h"

#define PCI_BUS_MAX 256
#define PCI_SLOT_MAX 32
#define PCI_FUNC_MAX 8

#define PCI_COMMAND_REGISTER 0x04

#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_BUS 0x4

#define PCI_BAR_BASE_OFFSET 0x10

#define PCI_CAPABILITY_MSI 0x05
#define PCI_CAPABILITY_PCIE 0x10
#define PCI_CAPABILITY_MSIX 0x11

static bool pci_verbose = false;

void pci_enable_verbose(){
    pci_verbose = true;
}

#define kprintfv(fmt, ...) \
    ({ \
        if (pci_verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

struct acpi_rsdp_t {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    // ACPI 2.0+:
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
}__attribute__((packed));

struct acpi_xsdt_t {
    char signature[4]; // "XSDT"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint64_t entries[]; // Array of table pointers
}__attribute__((packed));

struct acpi_mcfg_t {
    char signature[4]; // "MCFG"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint64_t reserved;
    struct {
        uint64_t base_address; // <<<< This is what you want
        uint16_t pci_segment_group_number;
        uint8_t start_bus_number;
        uint8_t end_bus_number;
        uint32_t reserved2;
    } __attribute__((packed)) allocations[];
} __attribute__((packed));

#define RSDP_SEARCH_START 0x000E0000
#define RSDP_SEARCH_END   0x00100000

bool initialized;

void* find_rsdp() {
    for (uint64_t addr = RSDP_SEARCH_START; addr < RSDP_SEARCH_END; addr += 16) {
        const char* sig = (const char*)(uintptr_t)addr;
        if (sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == ' ' &&
            sig[4] == 'P' && sig[5] == 'T' && sig[6] == 'R' && sig[7] == ' ') {
            
            struct acpi_rsdp_t* candidate = (struct acpi_rsdp_t*)(uintptr_t)addr;
            
            uint8_t sum = 0;
            uint8_t* p = (uint8_t*)candidate;
            for (int i = 0; i < 20; i++) sum += p[i];
            if (sum == 0) {
                if (candidate->revision >= 2) {
                    sum = 0;
                    for (uint32_t i = 0; i < candidate->length; i++) sum += p[i];
                    if (sum != 0) continue;
                }
                kprintf("[PCI] Found a suitable candidate");
                return candidate;
            }
        }
    }
    kprintf("[PCI] No candidate found");
    return 0x0;
}

void find_pci(){
    if (!initialized){
        initialized = true;
        for (uint64_t addr = PCI_BASE; addr < PCI_BASE + 0x10000000; addr += GRANULE_2MB)
            register_device_memory_2mb(addr, addr);
    }
}

uint64_t pci_make_addr(uint32_t bus, uint32_t slot, uint32_t func, uint32_t offset){
    return PCI_BASE
            | (((uint64_t)bus) << 20)
            | (((uint64_t)slot) << 15)
            | (((uint64_t)func) << 12) 
            | ((uint64_t)(offset & 0xFFF));
}

uint64_t pci_get_bar_address(uint64_t base, uint8_t offset, uint8_t index){
    if (!PCI_BASE)
        return 0;

    return base + offset + (index * 4);
}

uint64_t pci_read_address_bar(uintptr_t pci_addr, uint32_t bar_index){
    uint64_t bar_addr = pci_get_bar_address(pci_addr, PCI_BAR_BASE_OFFSET, bar_index);
    uint32_t original = read32(bar_addr);
    uint64_t full = original;
    if ((original & 0x6) == 0x4) {
        uint64_t bar_addr_hi = pci_get_bar_address(pci_addr, PCI_BAR_BASE_OFFSET, bar_index+1);
        uint64_t original_hi = read32(bar_addr_hi);
        full |= original_hi << 32;
    }
    return full & ~0xF;
}

uint64_t pci_setup_bar(uint64_t pci_addr, uint32_t bar_index, uint64_t *mmio_start, uint64_t *mmio_size) {
    uint64_t bar_addr = pci_get_bar_address(pci_addr, PCI_BAR_BASE_OFFSET, bar_index);
    uint32_t original = read32(bar_addr);

    write32(bar_addr, 0xFFFFFFFF);
    uint32_t bar_low = read32(bar_addr);
    kprintfv("[PCI] First bar size %x",bar_low);

    uint64_t size;
    if ((original & 0x6) == 0x4) {
        uint64_t bar_addr_hi = pci_get_bar_address(pci_addr, PCI_BAR_BASE_OFFSET, bar_index+1);
        uint32_t original_hi = read32(bar_addr_hi);

        kprintfv("[PCI] Original second bar %x",original_hi);

        write32(bar_addr_hi, 0xFFFFFFFF);
        uint32_t bar_high = read32(bar_addr_hi);

        kprintfv("[PCI] Second bar size %x",bar_high);

        uint64_t combined = ((uint64_t)bar_high << 32) | (bar_low & ~0xF);
        size = ~combined + 1;

        kprintfv("[PCI] Total bar size %x",size);

        uint64_t config_base = alloc_mmio_region(size);
        *mmio_start = config_base;
        *mmio_size = size;

        write32(bar_addr, config_base & 0xFFFFFFFF);
        write32(bar_addr_hi, config_base >> 32);

        uint64_t new_hi = read32(bar_addr_hi);
        uint64_t new_lo = read32(bar_addr);

        kprintfv("[PCI] Two registers %x > %x",new_hi,new_lo);
        uint64_t full = (new_hi << 32) | (new_lo & ~0xF);
        if (full != config_base){
            *mmio_start = full;
        }
    } else {
        uint32_t size32 = bar_low & ~0xF;
        size32 = ~size32 + 1;

        kprintfv("[PCI] Calculated bar size %x",size32);

        uint64_t config_base = alloc_mmio_region(size32);
        *mmio_start = config_base;
        *mmio_size = size32;

        write32(bar_addr, config_base & 0xFFFFFFFF);
    }

    write32(pci_addr + 0x04, read32(pci_addr + 0x04) | 0x2);

    return *mmio_start;
}

uint64_t find_pci_device(uint32_t vendor_id, uint32_t device_id) {

    if (!PCI_BASE)
        return 0;
    
    if (!initialized)
        find_pci();

    for (uint32_t bus = 0; bus < PCI_BUS_MAX; bus++) {
        for (uint32_t slot = 0; slot < PCI_SLOT_MAX; slot++) {
            for (uint32_t func = 0; func < PCI_FUNC_MAX; func++) {
                uint64_t device_address = pci_make_addr(bus, slot, func, 0x00);
                uint64_t vendor_device = read32(device_address);
                if ((vendor_device & 0xFFFF) == vendor_id && ((vendor_device >> 16) & 0xFFFF) == device_id) {

                    kprintf("[PCI] Found device at bus %i, slot %i, func %i", bus, slot, func);

                    return device_address;
                }// else if (((vendor_device >> 16) & 0xFFFF) != 0xFFFF)
                   //  kprintf("[PCI] VENDOR: %x DID: %x",(vendor_device & 0xFFFF),((vendor_device >> 16) & 0xFFFF));
            }
        }
    }
    kprintf("[PCI] Device not found. Vendor = %x Device = %x",vendor_id,device_id);
    return 0;
}

void dump_pci_config(uint64_t base) {
    kprintf("[PCI] Dumping PCI Configuration Space:");
    for (uint32_t offset = 0; offset < 0x40; offset += 4) {
        uint64_t val = read(base + offset);
        kprintf("Offset %x: %x",offset, val);
    }
}

void pci_enable_device(uint64_t pci_addr){
    uint32_t cmd = read16(pci_addr + PCI_COMMAND_REGISTER);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_REGISTER;
    write16(pci_addr + PCI_COMMAND_REGISTER,cmd);
}

void pci_register(uint64_t mmio_addr, uint64_t mmio_size){
    for (uint64_t addr = mmio_addr; addr <= mmio_addr + mmio_size; addr += GRANULE_4KB)
        register_device_memory(addr,addr);
}

#pragma region Interrupts

#define RP1_INT_PCI_MASK_CLR 0x4514
#define RP1_INT_PCI_BAR_L 0x4044
#define RP1_INT_PCI_BAR_H 0x4048
#define RP1_INT_PCI_CONFIG 0x404c

#define RP1_MSI_BAR_TRANSL_VIRT_L 0x402c
#define RP1_MSI_BAR_TRANSL_VIRT_H 0x4030
#define RP1_MSI_BAR_TRANSL_PHYS_L 0x40ac
#define RP1_MSI_BAR_TRANSL_PHYS_H 0x40b0

#define RP1_INT_BASE 0x1F00108000UL

#define IRQ_NUMBER__MASK	0x3FF
#define IRQ_EDGE_TRIG__MASK 0x400
#define IRQ_FROM_RP1__MASK  0x800

#define MSIX_CFG(irq)           (0x008 + (irq)*4)
#define MSIX_CFG_ENABLE     (1ULL << 0)
#define MSIX_CFG_TEST       (1ULL << 1)
#define MSIX_CFG_IACK       (1ULL << 2)
#define MSIX_CFG_IACK_ENABLE (1ULL << 3)

#define RP1_INT_SET            (RP1_INT_BASE + 0x800)
#define RP1_INT_CLR            (RP1_INT_BASE + 0xC00)
#define RP1_INT_STAT_LOW		(RP1_INT_BASE + 0x108)
#define RP1_INT_STAT_HIGH		(RP1_INT_BASE + 0x10C)

bool pci_setup_rp1() {
    uint64_t pci_addr = find_pci_device(0x1de4,1);
    // dump_pci_config(pci_addr);
    kprintf("RP1 %x",pci_addr);
    uint8_t cap_ptr = read8(pci_addr + 0x34);
    while (cap_ptr) {
        uint8_t cap_id = read8(pci_addr + cap_ptr);
        if (cap_id == PCI_CAPABILITY_MSIX){
            uint16_t msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            kprintf("Enabled? %x",(msg_ctrl >> 15) & 1);
            msg_ctrl |= (1 << 15); // Clear MSI-X Enable bit
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);
            msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            kprintf("Enabled? %x",(msg_ctrl >> 15) & 1);
        }
        cap_ptr = read8(pci_addr + cap_ptr + 1);
    }
    // uint64_t bar_addr = pci_get_bar_address(pci_addr, PCI_BAR_BASE_OFFSET, 1);
    // uint64_t bar_addr2 = pci_get_bar_address(pci_addr, PCI_BAR_BASE_OFFSET, 2);

    uintptr_t msi_pci_addr  = 0xFFFFFFF000UL;
    uintptr_t msi_phys_addr = 0x1000130000UL;

    //Translate pci address to phys address

    write32(pci_addr + RP1_MSI_BAR_TRANSL_VIRT_L, (msi_pci_addr & 0xFFFFFFFF) | 0x1C);
    write32(pci_addr + RP1_MSI_BAR_TRANSL_VIRT_H, (msi_pci_addr >> 32) & 0xFFFFFFFF);
    write32(pci_addr + RP1_MSI_BAR_TRANSL_PHYS_L, (msi_phys_addr & 0xFFFFFFFF) | 1);
    write32(pci_addr + RP1_MSI_BAR_TRANSL_PHYS_H, (msi_phys_addr >> 32) & 0xFFFFFFFF);

    write32(pci_addr + RP1_INT_PCI_MASK_CLR, UINT32_MAX);//Unmask all interrupts
    write32(pci_addr + RP1_INT_PCI_BAR_L, (msi_pci_addr & UINT32_MAX) | 1);//Set address and enable
    write32(pci_addr + RP1_INT_PCI_BAR_H, (msi_pci_addr >> 32) & UINT32_MAX);
    write32(pci_addr + RP1_INT_PCI_CONFIG, 0xffe03210);//ffe0 32 messages, 3210 arbitrary unique value

    for (int i = 0; i < 6; i++)
        kprintf("BAR %i = %x",i,pci_read_address_bar(pci_addr, i));
    kprintf("INTa? %i",read8 (pci_addr + 0x3D));
    kprintf("CMD %b",read16(pci_addr + PCI_COMMAND_REGISTER));

    return true;

}

bool pci_setup_msi_rp1(uint8_t irq_line, bool edge_triggered) {

    kprintf("Sanity 1 %i",read32(0x1F00108000));

    if (edge_triggered)
        write32(RP1_INT_CLR + MSIX_CFG(irq_line), MSIX_CFG_IACK_ENABLE);
    else
        write32(RP1_INT_SET + MSIX_CFG(irq_line), MSIX_CFG_IACK_ENABLE);

    kprintf("Sanity 2 %i",read32(RP1_INT_SET + MSIX_CFG(irq_line)));
    kprintf("Sanity 3 %i",read32(RP1_INT_CLR + MSIX_CFG(irq_line)));

    write32(RP1_INT_SET + MSIX_CFG(irq_line), MSIX_CFG_ENABLE);

    kprintf("Sanity 4 %i",read32(RP1_INT_SET + MSIX_CFG(irq_line)));

    write32(RP1_INT_SET + MSIX_CFG(irq_line), MSIX_CFG_TEST);

    kprintf("Sanity 5 %i",read32(RP1_INT_SET + MSIX_CFG(irq_line)));

    kprintf("Fired? %b", (uint64_t)read32(RP1_INT_STAT_HIGH) << 32 | read32(RP1_INT_STAT_LOW));

    return false;
}

bool pci_setup_msi(uint64_t pci_addr, uint8_t irq_line) {
    uint8_t cap_ptr = read8(pci_addr + 0x34);
    while (cap_ptr) {
        uint8_t cap_id = read8(pci_addr + cap_ptr);
        if (cap_id == PCI_CAPABILITY_MSI) { // MSI
            uint16_t msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            bool is_64bit = msg_ctrl & (1 << 7);

            write32(pci_addr + cap_ptr + 0x4, 0x8020040);
            int offset = 0x8;
            if (is_64bit) {
                write32(pci_addr + cap_ptr + 0x8, 0);
                offset += 4;
            }

            write16(pci_addr + cap_ptr + offset, MSI_OFFSET + irq_line);
            msg_ctrl |= 1; // enable MSI
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);
            return true;
        }
        cap_ptr = read8(pci_addr + cap_ptr + 1);
    }
    return false;
}

typedef struct {
    uint32_t msg_addr_low;
    uint32_t msg_addr_high;
    uint32_t msg_data;
    uint32_t vector_control;
} msix_table_entry;

bool pci_setup_msix(uint64_t pci_addr, msix_irq_line* irq_lines, uint8_t line_size) {

    uint8_t cap_ptr = read8(pci_addr + 0x34);
    while (cap_ptr) {
        uint8_t cap_id = read8(pci_addr + cap_ptr);
        if (cap_id == PCI_CAPABILITY_MSIX) { // MSI-X
            uint16_t msg_ctrl = read16(pci_addr + cap_ptr + 0x2);
            msg_ctrl &= ~(1 << 15); // Clear MSI-X Enable bit
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);
            uint16_t table_size = (msg_ctrl & 0x07FF) +1; // takes the 11 rightmost bits, its value is N-1, so add 1 to it for the full size

            if(line_size > table_size){
                kprintf("[PCI] MSI-X only supports %i interrupts, but you tried to add %i interrupts", table_size, line_size);
                return false;
            }

            uint32_t table_offset = read32(pci_addr + cap_ptr + 0x4);
            uint8_t bir = table_offset & 0x7;
            uint32_t table_addr_offset = table_offset & ~0x7;
            
            uint64_t table_addr = pci_read_address_bar(pci_addr, bir);

            if(!table_addr){
                uint64_t bar_size;
                pci_setup_bar(pci_addr, bir, &table_addr, &bar_size);
                kprintf("Setting up new bar for MSI-X %x + %x",table_addr, table_addr_offset);
            } else kprintf("Bar %i setup at %x + %x",bir, table_addr, table_addr_offset);
            
            msix_table_entry *msix_start = (msix_table_entry *)(uintptr_t)(table_addr + table_addr_offset);

            for (uint32_t i = 0; i < line_size; i++){
                msix_table_entry *msix_entry = msix_start + i;

                msix_irq_line irq_line = irq_lines[i];
                uint64_t addr_full = 0x8020040 + irq_line.addr_offset;

                msix_entry->msg_addr_low = addr_full & 0xFFFFFFFF;
                msix_entry->msg_addr_high = addr_full >> 32;
                msix_entry->msg_data = MSI_OFFSET + irq_line.irq_num;
                msix_entry->vector_control = msix_entry->vector_control & ~0x1; // all bits other then the last one are reserved, so don't chnage it, just set the last bit to 0
            }
            
            msg_ctrl |= (1 << 15); // MSI-X Enable
            msg_ctrl &= ~(1 << 14); // Clear Function Mask
            write16(pci_addr + cap_ptr + 0x2, msg_ctrl);

            break;
        }
        cap_ptr = read8(pci_addr + cap_ptr + 1);
    }

    return true;
}

uint8_t pci_setup_interrupts(uint64_t pci_addr, uint8_t irq_line, uint8_t amount){
    msix_irq_line irq_lines[amount];
    for (uint8_t i = 0; i < amount; i++)
        irq_lines[i] = (msix_irq_line){.addr_offset=0,.irq_num=irq_line+i};

    bool msix_ok = pci_setup_msix(pci_addr, irq_lines, amount);

    if(msix_ok){
        return 1;
    }

    bool msi_ok = true;
    for (uint8_t i = 0; i < amount; i++)
        msi_ok &= pci_setup_msi(pci_addr, irq_line+i);
    if(msi_ok){
        return 2;
    }
    
    return 0;
}
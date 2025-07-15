#include "irq.h"
#include "console/kio.h"
#include "memory/memory_access.h"
#include "process/scheduler.h"
#include "input/input_dispatch.h"
#include "input/xhci_types.h"
#include "pci.h"
#include "console/serial/uart.h"
#include "networking/network.h"
#include "hw/hw.h"

#define IRQ_TIMER 30
#define SLEEP_TIMER 27

extern void irq_el1_asm_handler();

static void gic_enable_irq(uint32_t irq, uint8_t priority, uint8_t cpu_target) {
    if (RPI_BOARD == 3){
        write32(GICD_BASE + 0x210, 1 << irq);
        return;
    }
    uint32_t reg_offset = (irq / 32) * 4;
    uint32_t bit = 1 << (irq % 32);

    uint32_t flag = read32(GICD_BASE + 0x100 + reg_offset);
    write32(GICD_BASE + 0x100 + reg_offset, flag | bit);

    write8(GICD_BASE + 0x800 + irq, cpu_target);
    write8(GICD_BASE + 0x400 + irq, priority);  

    uint32_t config_offset = (irq / 16) * 4;
    uint32_t config_shift = (irq % 16) * 2;
    uint32_t config = read32(GICD_BASE + 0xC00 + config_offset);
    config &= ~(3 << config_shift);
    config |= (2 << config_shift); // 0b10 = edge-triggered
    write32(GICD_BASE + 0xC00 + config_offset, config);
}

void irq_init() {
    if (RPI_BOARD != 3){
        write32(GICD_BASE, 0); // Disable Distributor
        write32(GICC_BASE, 0); // Disable CPU Interface
    }

    gic_enable_irq(IRQ_TIMER, 0x80, 0);
    gic_enable_irq(MSI_OFFSET + XHCI_IRQ, 0x80, 0);
    gic_enable_irq(MSI_OFFSET + NET_IRQ, 0x80, 0);
    gic_enable_irq(MSI_OFFSET + NET_IRQ + 1, 0x80, 0);
    gic_enable_irq(SLEEP_TIMER, 0x80, 0);

    if (RPI_BOARD != 3){
        write32(GICC_BASE + 0x004, 0xF0); //Priority

        write32(GICC_BASE, 1); // Enable CPU Interface
        write32(GICD_BASE, 1); // Enable Distributor

        kprintf_l("[GIC] GIC enabled");
    } else {
        kprintf_l("Interrupts initialized");
    }
}

void enable_interrupt() {
    asm volatile ("msr daifclr, #2");
    asm volatile ("isb");
}

void disable_interrupt(){
    asm volatile ("msr daifset, #2");
    asm volatile ("isb");
}

void irq_el1_handler() {
    save_context_registers();
    save_return_address_interrupt();
    if (ksp != 0){
        asm volatile ("mov sp, %0" :: "r"(ksp));
    }
    uint32_t irq;
    if (RPI_BOARD == 3){
        irq = 31 - __builtin_clz(read32(GICD_BASE + 0x204));
    } else irq = read32(GICC_BASE + 0xC);

    if (irq == IRQ_TIMER) {
        if (RPI_BOARD != 3) write32(GICC_BASE + 0x10, irq);
        switch_proc(INTERRUPT);
    } else if (irq == MSI_OFFSET + XHCI_IRQ){
        handle_input_interrupt();
        if (RPI_BOARD != 3) write32(GICC_BASE + 0x10, irq);
        process_restore();
    } else if (irq == SLEEP_TIMER){
        wake_processes();
        if (RPI_BOARD != 3) write32(GICC_BASE + 0x10, irq);
        process_restore();
    } else if (irq == MSI_OFFSET + NET_IRQ){
        network_handle_download_interrupt();
        if (RPI_BOARD != 3) write32(GICC_BASE + 0x10, irq);
        process_restore();
    }  else if (irq == MSI_OFFSET + NET_IRQ + 1){
        network_handle_upload_interrupt();
        if (RPI_BOARD != 3) write32(GICC_BASE + 0x10, irq);
        process_restore();
    } else {
        kprintf_raw("[GIC error] Received unknown interrupt");
        if (RPI_BOARD != 3) write32(GICC_BASE + 0x10, irq);
        process_restore();
    }
}

#include "gic.h"
#include "console/kio.h"
#include "ram_e.h"
#include "process/scheduler.h"
#include "input/xhci_types.h"
#include "pci.h"
#include "input/xhci.h"

#define IRQ_TIMER 30

static uint64_t _msecs;

extern void irq_el1_asm_handler();

static void gic_enable_irq(uint32_t irq, uint8_t priority, uint8_t cpu_target) {
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

void gic_init() {
    write8(GICD_BASE, 0); // Disable Distributor
    write8(GICC_BASE, 0); // Disable CPU Interface

    gic_enable_irq(IRQ_TIMER, 0x80, 0);
    gic_enable_irq(MSI_OFFSET + XHCI_IRQ, 0x80, 0);

    write32(GICD_BASE + 0x000, 0x1);

    write32(GICC_BASE + 0x000, 0x1); //Priority
    write32(GICC_BASE + 0x004, 0xF0); //Priority

    write8(GICC_BASE, 1); // Enable CPU Interface
    write8(GICD_BASE, 1); // Enable Distributor

    kprintf("[GIC] GIC enabled\n");
}

void timer_reset() {
    uint64_t freq;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = (freq * _msecs)/1000;
    asm volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
}

void timer_enable() {
    uint64_t val = 1;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(val));
    asm volatile ("msr cntkctl_el1, %0" :: "r"(val));
}

void permanent_disable_timer(){
    uint64_t ctl = 0;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
}

void timer_init(uint64_t msecs) {
    _msecs = msecs;
    timer_reset();
    timer_enable();
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
    uint32_t irq = read32(GICC_BASE + 0xC);

    if (irq == IRQ_TIMER) {
        timer_reset();
        write32(GICC_BASE + 0x10, irq);
        switch_proc(INTERRUPT);
    } else if (irq == MSI_OFFSET + XHCI_IRQ){
        xhci_handle_interrupt();
        write32(GICC_BASE + 0x10, irq);
        process_restore();
    } else {
        kprintf_raw("[GIC error]Received unknown interrupt");
        write32(GICC_BASE + 0x10, irq);
        process_restore();
    }
}
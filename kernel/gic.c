#include "gic.h"
#include "console/kio.h"
#include "ram_e.h"
#include "process/scheduler.h"

#define IRQ_TIMER 30

static uint64_t _msecs;

extern void irq_el1_asm_handler();

void gic_init() {
    uint64_t current_el;
    asm volatile ("mrs %0, CurrentEL" : "=r"(current_el));
    printf("[GIC INIT] CurrentEL: %h\n", current_el);

    write8(GICD_BASE, 0); // Disable Distributor
    write8(GICC_BASE, 0); // Disable CPU Interface

    uint32_t flag = read32(GICD_BASE + 0x100 + (IRQ_TIMER / 32) * 4);
    write32(GICD_BASE + 0x100 + (IRQ_TIMER / 32) * 4, flag | (1 << (IRQ_TIMER % 32))); // Enable IRQ
    write32(GICD_BASE + 0x800 + (IRQ_TIMER / 4) * 4, 1 << ((IRQ_TIMER % 4) * 8)); // Target CPU 0
    write32(GICD_BASE + 0x400 + (IRQ_TIMER / 4) * 4, 0); // Priority 0

    write16(GICC_BASE + 0x004, 0xF0); // Priority Mask Register

    write8(GICC_BASE, 1); // Enable CPU Interface
    write8(GICD_BASE, 1); // Enable Distributor

    printf("[GIC INIT] GIC enabled\n");
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

void timer_init(uint64_t msecs) {
    _msecs = msecs;
    timer_reset();
    timer_enable();
}

void enable_interrupt() {
    asm volatile ("msr daifclr, #2");
    printf("[INTERRUPTS] Global interrupts enabled\n");
}

void irq_el1_handler() {
    save_context_registers();
    save_return_address_interrupt();
    uint32_t irq = read32(GICC_BASE + 0xC);

    if (irq == IRQ_TIMER) {
        timer_reset();
        write32(GICC_BASE + 0x10, irq);
        switch_proc(INTERRUPT);
        restore_return_address_interrupt();
        return;
    }

    // printf(">>> Unhandled IRQ: %h\n", irq);
}
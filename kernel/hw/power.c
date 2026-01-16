#include "power.h"

#include "hw/hw.h"
#include "std/memory_access.h"
#include "exceptions/irq.h"
#include "types.h"

static inline uint64_t hvc_call(uint64_t fid, uint64_t x1, uint64_t x2, uint64_t x3){
    register uint64_t r0 asm("x0") = fid;
    register uint64_t r1 asm("x1") = x1;
    register uint64_t r2 asm("x2") = x2;
    register uint64_t r3 asm("x3") = x3;
    asm volatile("hvc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) :: "x4", "x5", "x6", "x7", "memory");
    return r0;
}

static inline void rpi_full_reset(){
    uintptr_t base = PM_BASE;
    uintptr_t wdog = base + 0x24u;
    uintptr_t rstc = base + 0x1cu;
    uint32_t pass = 0x5a000000u;
    write32(wdog, pass | (10u & 0xffffu));
    uint32_t val = read32(rstc);
    val &= ~0x30u;
    val |= pass | 0x20u;
    write32(rstc, val);
    while (1) asm volatile("wfi");
}

static inline void rpi_mark_halt(){
    uintptr_t base = PM_BASE;
    uintptr_t rsts = base + 0x20u;
    uint32_t pass = 0x5a000000u;
    uint32_t val = read32(rsts);
    val |= pass | 0x00000555u;
    write32(rsts, val);
}

static inline void psci_reset(){
    (void)hvc_call(0x84000009u, 0, 0, 0);
    while (1) asm volatile("wfi");
}

static inline void psci_off(){
    (void)hvc_call(0x84000008u, 0, 0, 0);
    while (1) asm volatile("wfi");
}

void hw_shutdown(shutdown_mode mode){
    disable_interrupt();

    if (RPI_BOARD != 0){
        //TODO raspi shutdown isn't tested
        //(im not fully confident raspi is correct)
        if (mode == SHUTDOWN_REBOOT) rpi_full_reset();
        if (mode == SHUTDOWN_POWEROFF){
            rpi_mark_halt();
            rpi_full_reset();
        }
    }

    if (BOARD_TYPE == 1){
        if (mode == SHUTDOWN_REBOOT) psci_reset();
        if (mode == SHUTDOWN_POWEROFF) psci_off();
    }

    while (1) asm volatile("wfi");
}

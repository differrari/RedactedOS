#include "gpio.h"
#include "hw/hw.h"
#include "async.h"
#include "std/memory_access.h"
#include "memory/mmu.h"

void reset_gpio(){
    register_device_memory(GPIO_BASE, GPIO_BASE);
    write32(GPIO_BASE + GPIO_PIN_BASE + 0x94, 0x0);
    delay(150);
}

void enable_gpio_pin(uint8_t pin){
    uint32_t v = read32(GPIO_BASE + GPIO_PIN_BASE + 0x98);
    write32(GPIO_BASE + GPIO_PIN_BASE + 0x98, v | (1 << pin));
}
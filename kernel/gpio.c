#include "gpio.h"
#include "hw/hw.h"
#include "async.h"
#include "memory/memory_access.h"

void reset_gpio(){
    write32(GPIO_BASE + GPIO_PIN_BASE + 0x94, 0x0);
    delay(150);
}

void enable_gpio_pin(uint8_t pin){
    uint32_t v = read32(GPIO_BASE + GPIO_PIN_BASE + 0x98);
    write32(GPIO_BASE + GPIO_PIN_BASE + 0x98, v | (1 << pin));
}
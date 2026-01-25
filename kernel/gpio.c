#include "gpio.h"
#include "hw/hw.h"
#include "async.h"
#include "memory/mmu.h"
#include "std/memory_access.h"

void reset_gpio(){
	register_device_memory(GPIO_BASE, GPIO_BASE);
    write32(GPIO_BASE + GPIO_PIN_BASE + 0x94, 0x0);
    delay(150);
}

#define GPIOFSEL0 0
#define GPIOSET0  0x1C
#define GPIOCLR0 0x28
#define GPIOPUPPDN0 0xE4

#define GPIO_PULL_NONE 0
#define GPIO_PULL_DOWN 2
#define GPIO_PULL_UP 1

#define GPIO_FUNCTION_IN 0
#define GPIO_FUNCTION_OUT 1
#define GPIO_FUNCTION_ALT5 2
#define GPIO_FUNCTION_ALT3 7
#define GPIO_FUNCTION_ALT0 4

bool call_gpio(unsigned long pin, unsigned long value, uint32_t base, unsigned long field_size)
{
    unsigned long field_mask = (1 << field_size) - 1;

    if (pin > field_mask && value > field_mask)
        return false;

    unsigned long num_fields = 32 / field_size;
    unsigned long regist = GPIO_BASE + base + ((pin / num_fields) * 4);
    unsigned long shift = (pin % num_fields) * field_size;

    unsigned long finalval = read32(regist);
    finalval &= ~(field_mask << shift);
    finalval |= value << shift;

    write32(regist, finalval);

    return true;
}

bool pull_gpio(unsigned long pin, uint32_t pull)
{
    return call_gpio(pin, pull, GPIOPUPPDN0, 2);
}

bool set_gpio_func(unsigned long pin, uint32_t func)
{
    return call_gpio(pin, func, GPIOFSEL0, 3);
}

void enable_gpio_pin(uint8_t pin){
    pull_gpio(pin, GPIO_PULL_NONE);
    set_gpio_func(pin, GPIO_FUNCTION_ALT0);
    pull_gpio(pin, GPIO_PULL_NONE);
}
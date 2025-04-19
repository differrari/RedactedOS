#include "uart.h"
#include "mmio.h"
#include "pci.h"

#define FW_CFG_DATA  0x09020000
#define FW_CFG_CTL   (FW_CFG_DATA + 0x8)
#define FW_CFG_DMA   (FW_CFG_DATA + 0x10)

#define FW_LIST_DIRECTORY 0x19

#define FORMAT_SIZE 32

#define FW_CFG_DMA_READ 0x2
#define FW_CFG_DMA_SELECT 0x8
#define FW_CFG_DMA_WRITE 0x10
#define FW_CFG_DMA_ERROR 0x1

typedef struct {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;

    uint32_t stride;
    uint32_t size;
} fb_structure;

struct fw_cfg_file {
    uint32_t size;
    uint16_t selector;
    uint16_t reserved;
    char name[56];
}__attribute__((packed));

struct fw_cfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
}__attribute__((packed));

uint64_t fb_ptr;

uint32_t width;
uint32_t height;
uint32_t bpp;
uint32_t stride;

bool fw_cfg_check(){
    return read64(FW_CFG_DATA) == 0x554D4551;
}

void rfb_clear(){

}

void fw_cfg_dma_read(void* dest, uint32_t size, uint32_t ctrl) {
    struct fw_cfg_dma_access access = {
        .address = __builtin_bswap64((uint64_t)dest),
        .length = __builtin_bswap32(size),
        .control = __builtin_bswap32(ctrl),
    };

    write64(FW_CFG_DMA, __builtin_bswap64((uint64_t)&access));

    __asm__("ISB");

    uart_puts("Wrote access command");

    while (__builtin_bswap32(access.control) & ~0x1) {}
    
}

void fw_find_file() {

    uint32_t count;
    fw_cfg_dma_read(&count, sizeof(count), (FW_LIST_DIRECTORY << 16) | FW_CFG_DMA_SELECT | FW_CFG_DMA_READ);

    count = __builtin_bswap32(count);

    uart_puts("There are ");
    uart_puthex(count);
    uart_puts(" values in directory\n");

    for (uint32_t i = 0; i < count; i++) {
        struct fw_cfg_file file;

        fw_cfg_dma_read(&file, sizeof(file), FW_CFG_DMA_READ);

        uint32_t size = __builtin_bswap32(file.size);
        uint16_t selector = __builtin_bswap16(file.selector);

        uart_puts("File ");
        uart_puthex(i);
        uart_puts(": name=");
        for (int j = 0; j < 56 && file.name[j]; j++) {
            uart_putc(file.name[j]);
        }
        uart_puts(", size=");
        uart_puthex(size);
        uart_puts(", selector=");
        uart_puthex(selector);
        uart_putc('\n');
    }
}

bool rfb_init() {

    if (!fw_cfg_check()){
        uart_puts("Wrong FW_CFG config");
        return false;
    }

    width = 1024;
    height = 768;
    bpp = 4;
    stride = bpp * width;

    fb_ptr = alloc(width * height * bpp);

    fb_structure fb = {
        .addr = fb_ptr,
        .width = width,
        .height = height,
        .bpp = bpp,
        .stride = stride,
        .size = stride * height
    };

    fw_find_file();
    
    return false;
}

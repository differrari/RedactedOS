#include "fw_cfg.h"
#include "console/kio.h"
#include "std/memory_access.h"
#include "memory/mmu.h"
#include "async.h"
#include "sysregs.h"
#include "std/string.h"
#include "memory/addr.h"

#define FW_CFG_DATA  0x09020000
#define FW_CFG_CTL   (FW_CFG_DATA + 0x8)
#define FW_CFG_DMA   (FW_CFG_DATA + 0x10)

#define FW_CFG_DMA_READ 0x2
#define FW_CFG_DMA_SELECT 0x8
#define FW_CFG_DMA_WRITE 0x10
#define FW_CFG_DMA_ERROR 0x1

#define FW_LIST_DIRECTORY 0x19


static bool checked = false;

struct fw_cfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
}__attribute__((packed));

bool fw_cfg_check(){
    if (checked) return true;
    uintptr_t va = PHYS_TO_VIRT(FW_CFG_DATA);
    register_device_memory_dmap(va);
    checked = read64(va) == 0x554D4551;
    if (!checked) mmu_unmap(va, FW_CFG_DATA);
    return checked;
}

void fw_cfg_dma_operation(void* dest, uint32_t size, uint32_t ctrl) {
    struct fw_cfg_dma_access access = {
        .address = __builtin_bswap64(pt_va_to_pa(dest)),
        .length = __builtin_bswap32(size),
        .control = __builtin_bswap32(ctrl),
    };

    write64(PHYS_TO_VIRT(FW_CFG_DMA), __builtin_bswap64(pt_va_to_pa(&access)));
    
    __asm__("isb");

    if (!wait(&access.control, __builtin_bswap32(~0x1), false, 2000)){
        kprintf("[FW_CFG error] failed to communicate with fw_cfg");
    }
    
}

void fw_cfg_dma_read(void* dest, u32 size, u32 ctrl){
    if (!fw_cfg_check())
        return;

    fw_cfg_dma_operation(dest, size, (ctrl << 16) | FW_CFG_DMA_SELECT | FW_CFG_DMA_READ);
}

void fw_cfg_dma_write(void* dest, u32 size, u32 ctrl){
    if (!fw_cfg_check())
        return;

    fw_cfg_dma_operation(dest, size, (ctrl << 16) | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
}

bool fw_find_file(const char* search, struct fw_cfg_file *file) {

    if (!fw_cfg_check())
        return false;

    u32 count = 0;
    fw_cfg_dma_read(&count, sizeof(count), FW_LIST_DIRECTORY);

    count = __builtin_bswap32(count);

    for (u32 i = 0; i < count; i++) {

        fw_cfg_dma_operation(file, sizeof(struct fw_cfg_file), FW_CFG_DMA_READ);

        file->size = __builtin_bswap32(file->size);
        file->selector = __builtin_bswap16(file->selector);

        if (strcmp(file->name, search) == 0){
            kprintf("Found device at selector %x", file->selector);
            return true;
        }
    }

    return false;
}
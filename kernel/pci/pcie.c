#include "pcie.h"
#include "hw/hw.h"
#include "memory/memory_access.h"
#include "console/kio.h"
#include "memory/mmu.h"
#include "async.h"
#include "sysregs.h"
#include "pcie_types.h"

#define REG_SET(base, reg, field, val) pcie_reg_set(base + reg, reg##_##field##_MASK, reg##_##field##_SHIFT, val)

#define INSERT_FIELD(val, reg, field, field_val) ((val & ~reg##_##field##_MASK) | (reg##_##field##_MASK & (field_val << reg##_##field##_SHIFT)))

bool pcie_link_up(uintptr_t base){
    u32 val = read32(base + PCIE_STATUS);
    kprintf("Link status %x",val);
    return (val & STATUS_PCIE_DL_ACTIVE_MASK) >> STATUS_PCIE_DL_ACTIVE_SHIFT && (val & STATUS_PCIE_PHYLINKUP_MASK) >> STATUS_PCIE_PHYLINKUP_SHIFT;
}

void pcie_reg_set(uptr addr, u32 mask, u32 shift, u32 val){
    uint32_t sw = read32(addr);

    sw = (sw & ~mask) | ((val << shift) & mask);
    
    write32(addr, sw);
}

int ilog2(u64 v)
{
	int l = 0;
	while (((u64) 1 << l) < v)
		l++;
	return l;
}

int encode_ibar_size(u64 size)
{
	int log2_in = ilog2(size);

	if (log2_in >= 12 && log2_in <= 15)
		return (log2_in - 12) + 0x1c;
	else if (log2_in >= 16 && log2_in <= 37)
		return log2_in - 15;
	return 0;
}

bool init_hostbridge(){
    
    for (int i = 0; i < 10; i++){
        register_device_memory(PCI_BASE + (i * 0x1000), PCI_BASE + (i * 0x1000));
    }
    
    uintptr_t base = VIRT_TO_PHYS(PCI_BASE);
    kprint("Starting");
    
    REG_SET(base, PCIE_RGR1_SW_INIT_1, INIT, 1);
    REG_SET(base, PCIE_RGR1_SW_INIT_1, PERST, 1);
    
    kprintf("Reset");
    
    delay(100);
    kprintf("Delay");
    
    REG_SET(base, PCIE_RGR1_SW_INIT_1, INIT, 0);
    kprintf("Hard Debug");
    
    REG_SET(base, PCIE_MISC_HARD_PCIE_HARD_DEBUG, SERDES_IDDQ, 0);

	delay(200);
	
	kprintf("Configure");
    
    u32 tmp = read32(base + PCIE_MISC_MISC_CTRL);
    tmp = INSERT_FIELD(tmp, PCIE_MISC_MISC_CTRL, SCB_ACCESS_EN, 1);
    tmp = INSERT_FIELD(tmp, PCIE_MISC_MISC_CTRL, MAX_BURST_SIZE, 1);
    tmp = INSERT_FIELD(tmp, PCIE_MISC_MISC_CTRL, CFG_READ_UR_MODE, 1);
    tmp = INSERT_FIELD(tmp, PCIE_MISC_MISC_CTRL, MAX_BURST_SIZE, BURST_SIZE_128);//256 pi 5
    write32(base + PCIE_MISC_MISC_CTRL, tmp);
    
    size_t rc_bar2_size = MEM_PCIE_RANGE_SIZE;
    uintptr_t rc_bar2_offset = MEM_PCIE_RANGE_START;
    
    tmp = rc_bar2_offset & UINT32_MAX;
    tmp = INSERT_FIELD(tmp, PCIE_MISC_RC_BAR2_CONFIG_LO, SIZE, encode_ibar_size(rc_bar2_size));
    write32(base + PCIE_MISC_RC_BAR2_CONFIG_LO, tmp);
	write32(base + PCIE_MISC_RC_BAR2_CONFIG_HI, (rc_bar2_offset >> 32) & UINT32_MAX);
	
	REG_SET(base, PCIE_MISC_MISC_CTRL, SCB0_SIZE, 0xf);
	
	REG_SET(base, PCIE_MISC_RC_BAR1_CONFIG_LO, SIZE, 0);
	REG_SET(base, PCIE_MISC_RC_BAR3_CONFIG_LO, SIZE, 0);
	
	write32(base + PCIE_INTR2_CPU_BASE + CLR, 0xffffffff);
	write32(base + PCIE_MSI_INTR2_MASK_SET, 0xffffffff);
	write32(base + PCIE_MSI_INTR2_CLR, 0xffffffff);
    
	/* disable the PCIe->GISB memory window (RC_BAR1) */
	tmp = read32(base + PCIE_MISC_RC_BAR1_CONFIG_LO);
	tmp &= ~PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK;
	write32(base + PCIE_MISC_RC_BAR1_CONFIG_LO, tmp);
    
	/* disable the PCIe->SCB memory window (RC_BAR3) */
	tmp = read32(base + PCIE_MISC_RC_BAR3_CONFIG_LO);
	tmp &= ~PCIE_MISC_RC_BAR3_CONFIG_LO_SIZE_MASK;
	write32(base + PCIE_MISC_RC_BAR3_CONFIG_LO, tmp);
    
    kprintf("Clearing perst");
    
    REG_SET(base, PCIE_RGR1_SW_INIT_1, PERST, 0);
    
    delay(100);

    kprintf("Link stat now %i",pcie_link_up(base));
    
    for (int i = 0; i < 5 && !pcie_link_up(base); i++){
        kprint("Waiting for link...");
		delay(5);
    }
       
	if (!pcie_link_up(base)) {
		kprintf("PCIe BRCM: link down");
		return false;
	}
    
    return true;
}
//Adapted from https://github.com/rsta2/circle/blob/master/lib/bcmpciehostbridge.cpp

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

#define READ_FIELD(val, reg, field) ((val & reg##_##field##_MASK) >> reg##_##field##_SHIFT)

static uptr base;
static u64 pcie_addr;
static u64 cpu_addr;
static u64 win_size;

bool pcie_link_up(uintptr_t base){
    u32 val = read32(base + PCIE_STATUS);
    kprintf("Link status %x",val);
    return (val & STATUS_PCIE_DL_ACTIVE_MASK) >> STATUS_PCIE_DL_ACTIVE_SHIFT && (val & STATUS_PCIE_PHYLINKUP_MASK) >> STATUS_PCIE_PHYLINKUP_SHIFT;
}

void pcie_reg_set(uptr addr, u32 mask, u32 shift, u32 val){
    uint32_t sw = read32(addr);

    sw = (sw & ~mask) | ((val << shift) & mask);
    
    write32(addr, sw);
    
    (void)read32(addr);
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

int cfg_index(int busnr, int devfn, int reg)
{
	return    ((PCI_SLOT(devfn) & 0x1f) << PCIE_SLOT_SHIFT)
		| ((PCI_FUNC(devfn) & 0x07) << PCIE_FUNC_SHIFT)
		| (busnr << PCIE_BUSNUM_SHIFT)
		| (reg & ~3);
}

uptr pcie_map_conf(unsigned busnr, unsigned devfn, int where)
{
	/* Accesses to the RC go right to the RC registers if slot==0 */
	if (busnr == 0)
		return PCI_SLOT(devfn) ? 0 : base + where;

	/* For devices, write to the config space index register */
	int idx = cfg_index(busnr, devfn, 0);
	write32(base + PCIE_EXT_CFG_INDEX, idx);
	return base + PCIE_EXT_CFG_DATA + where;
}

bool enable_bridge(){
	uptr conf = pcie_map_conf(0, PCI_DEVFN(0, 0), 0);
	if (!conf){
        kprint("[PCIe error] no config to enable bridge");
		return false;
	}
    
	if (read32(conf + PCI_CLASS_REVISION) >> 8 != 0x060400 || read8(conf + PCI_HEADER_TYPE) != PCI_HEADER_TYPE_BRIDGE){
	    kprintf("[PCIe error] wrong class revision or header type");
		return false;
	}
    
	write8(conf + PCI_CACHE_LINE_SIZE, 64/4);
    
	write8(conf + PCI_SECONDARY_BUS, 1);
	write8(conf + PCI_SUBORDINATE_BUS, 1);
    
	write16(conf + PCI_MEMORY_BASE, pcie_addr >> 16);
	write16(conf + PCI_MEMORY_LIMIT, pcie_addr >> 16);
    
	write8(conf + PCI_BRIDGE_CONTROL, PCI_BRIDGE_CTL_PARITY);
    
	u8 cap = read8(conf + BRCM_PCIE_CAP_REGS + PCI_CAP_LIST_ID);
	if (cap != PCI_CAP_ID_EXP){
	    kprintf("Wrong capability id %i",cap);
	    return false;
	}
	write8(conf + BRCM_PCIE_CAP_REGS + PCI_EXP_RTCTL, PCI_EXP_RTCTL_CRSSVE);
    
	write16(conf + PCI_COMMAND,   PCI_COMMAND_MEMORY
				     | PCI_COMMAND_MASTER
				     | PCI_COMMAND_PARITY
				     | PCI_COMMAND_SERR);
    
	return true;
}

bool init_hostbridge(){
    
    for (int i = 0; i < 10; i++){
        register_device_memory_dmap(PCI_BASE + (i * 0x1000));
    }
    
    base = VIRT_TO_PHYS(PCI_BASE);
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
    
    for (int i = 0; i < 5 && !pcie_link_up(base); i++){
        kprint("Waiting for link...");
		delay(5);
    }
       
	if (!pcie_link_up(base)) {
		kprintf("PCIe BRCM: link down");
		return false;
	}
	
	u32 val = read32(base + PCIE_MISC_PCIE_STATUS);

	if (!READ_FIELD(val, PCIE_MISC_PCIE_STATUS, PCIE_PORT)){
	    kprintf("Wrong status field %i",val);
	}
	
	u64 cpu_addr_mb, limit_addr_mb;
	
	pcie_addr = MEM_PCIE_RANGE_PCIE_START;
	cpu_addr = MEM_PCIE_RANGE_START;
	win_size = MEM_PCIE_RANGE_SIZE;

	/* Set the m_base of the pcie_addr window */
	write32(base + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO, lo32(pcie_addr));
	write32(base + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI, hi32(pcie_addr));

	cpu_addr_mb = cpu_addr >> 20;
	limit_addr_mb = (cpu_addr + win_size - 1) >> 20;

	/* Write the addr m_base low register */
	REG_SET(base,
			   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT,
			   BASE, cpu_addr_mb);
	/* Write the addr limit low register */
	REG_SET(base,
			   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT,
			   LIMIT, limit_addr_mb);

	/* Write the cpu addr high register */
	tmp = (u32)(cpu_addr_mb >> PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_NUM_MASK_BITS);
	REG_SET(base,
			   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI,
			   BASE, tmp);
	/* Write the cpu limit high register */
	tmp = (u32)(limit_addr_mb >>
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_NUM_MASK_BITS);
	REG_SET(base,
			   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI,
			   LIMIT, tmp);
	
	REG_SET(base, PCIE_RC_CFG_PRIV1_ID_VAL3, CLASS_CODE, 0x060400);

	/* PCIe->SCB endian mode for BAR */
	/* field ENDIAN_MODE_BAR2 = DATA_ENDIAN */
	REG_SET(base, PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1, ENDIAN_MODE_BAR2, 0);

	/*
	 * Refclk from RC should be gated with CLKREQ# input when ASPM L0s,L1
	 * is enabled =>  setting the CLKREQ_DEBUG_ENABLE field to 1.
	 */
	REG_SET(base, PCIE_MISC_HARD_PCIE_HARD_DEBUG, CLKREQ_DEBUG_ENABLE, 1);
	
	return enable_bridge();
}
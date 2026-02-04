#include "pcie.h"
#include "hw/hw.h"
#include "memory/memory_access.h"
#include "console/kio.h"
#include "memory/mmu.h"
#include "async.h"

#define PCIE_STATUS                                             0x4068

#define PCIE_MISC_MISC_CTRL                                     0x4008

#define  MISC_CTRL_SCB_ACCESS_EN_MASK                           0x1000
#define  MISC_CTRL_CFG_READ_UR_MODE_MASK                        0x2000
#define  MISC_CTRL_MAX_BURST_SIZE_MASK                          0x300000
#define  MISC_CTRL_MAX_BURST_SIZE_128                           0x0
#define  MISC_CTRL_SCB0_SIZE_MASK                               0xf8000000
#define PCIE_RGR1_SW_INIT_1 0x9210

#define PCIE_MISC_HARD_PCIE_HARD_DEBUG                          0x4304
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK         0x08000000
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_SHIFT        0x1b
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_L1SS_ENABLE_MASK  0x00200000
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK 0x00000002

#define PCIE_RGR1_SW_INIT_1_INIT_MASK                           0x2
#define PCIE_RGR1_SW_INIT_1_PERST_MASK                          0x1

bool pcie_link_up(){
    return read32(PCI_BASE + PCIE_STATUS) == 1;
}

bool init_hostbridge(){
    
    register_device_memory_2mb(PCI_BASE, PCI_BASE);
    
    if (pcie_link_up()){
        kprint("Link already configured");
        return false;
    }
    
    uint32_t *sw_init = (uint32_t*)PCI_BASE + PCIE_RGR1_SW_INIT_1;
    
    //Reset
    *sw_init |= PCIE_RGR1_SW_INIT_1_INIT_MASK | PCIE_RGR1_SW_INIT_1_PERST_MASK;
    
    kprintf("Reset");
    
    delay(100);
    
    kprintf("Delay");
    
    *sw_init &= ~PCIE_RGR1_SW_INIT_1_INIT_MASK;
    kprintf("Hard Debug");
    
    u32 *tmp = (u32*)(PCI_BASE + PCIE_MISC_HARD_PCIE_HARD_DEBUG);
	*tmp &= ~PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK;

	delay(200);
	
	kprintf("Configure");
    
    u32 *misc_ctrl = (u32*)(PCI_BASE + PCIE_MISC_MISC_CTRL);
    *misc_ctrl &= ~MISC_CTRL_MAX_BURST_SIZE_MASK;
    *misc_ctrl |=   MISC_CTRL_SCB_ACCESS_EN_MASK |
                    MISC_CTRL_CFG_READ_UR_MODE_MASK |
                    MISC_CTRL_MAX_BURST_SIZE_128;
    
    kprintf("Clearing perst");
    
    *sw_init &= ~PCIE_RGR1_SW_INIT_1_PERST_MASK;

    kprintf("Link stat now %i",pcie_link_up());
    
    for (int i = 0; i < 100 && !pcie_link_up(); i += 5)
		delay(5);
       
	if (!pcie_link_up()) {
		kprintf("PCIe BRCM: link down");
		return false;
	}
    
    return true;
    
 //    pci_get_dma_regions(dev, &region, 0);
	// rc_bar2_offset = region.bus_start - region.phys_start;
	// rc_bar2_size = 1ULL << fls64(region.size - 1);
    
	// tmp = lower_32_bits(rc_bar2_offset);
	// u32p_replace_bits(&tmp, brcm_pcie_encode_ibar_size(rc_bar2_size),
	// 		  RC_BAR2_CONFIG_LO_SIZE_MASK);
	// writel(tmp, base + PCIE_MISC_RC_BAR2_CONFIG_LO);
	// writel(upper_32_bits(rc_bar2_offset),
	//        base + PCIE_MISC_RC_BAR2_CONFIG_HI);
    
	// scb_size_val = rc_bar2_size ?
	// 	       ilog2(rc_bar2_size) - 15 : 0xf; /* 0xf is 1GB */
    
	// tmp = readl(base + PCIE_MISC_MISC_CTRL);
	// u32p_replace_bits(&tmp, scb_size_val,
	// 		  MISC_CTRL_SCB0_SIZE_MASK);
	// writel(tmp, base + PCIE_MISC_MISC_CTRL);
    
	// /* Disable the PCIe->GISB memory window (RC_BAR1) */
	// clrbits_le32(base + PCIE_MISC_RC_BAR1_CONFIG_LO,
	// 	     RC_BAR1_CONFIG_LO_SIZE_MASK);
    
	// /* Disable the PCIe->SCB memory window (RC_BAR3) */
	// clrbits_le32(base + PCIE_MISC_RC_BAR3_CONFIG_LO,
	// 	     RC_BAR3_CONFIG_LO_SIZE_MASK);
    
	// /* Mask all interrupts since we are not handling any yet */
	// write32(PCI_BASE + PCIE_MSI_INTR2_MASK_SET,0xffffffff);
    
	// /* Clear any interrupts we find on boot */
	// write32(PCI_BASE + PCIE_MSI_INTR2_CLR,0xffffffff);
    
	// // if (pcie->gen)
	// // 	brcm_pcie_set_gen(pcie, pcie->gen);
    
	// /* Unassert the fundamental reset */
	// clrbits_le32(pcie->base + PCIE_RGR1_SW_INIT_1,
	// 	     PCIE_RGR1_SW_INIT_1_PERST_MASK);
    
	// /*
	//  * Wait for 100ms after PERST# deassertion; see PCIe CEM specification
	//  * sections 2.2, PCIe r5.0, 6.6.1.
	//  */
	// delay(100);
    
	// /* Give the RC/EP time to wake up, before trying to configure RC.
	//  * Intermittently check status for link-up, up to a total of 100ms.
	//  */
	// for (i = 0; i < 100 && !brcm_pcie_link_up(pcie); i += 5)
	// 	delay(5);
    
	// if (!brcm_pcie_link_up(pcie)) {
	// 	printf("PCIe BRCM: link down\n");
	// 	return false;
	// }
    
	// if (!brcm_pcie_rc_mode(pcie)) {
	// 	printf("PCIe misconfigured; is in EP mode\n");
	// 	return false;
	// }
    
	// for (i = 0; i < hose->region_count; i++) {
	// 	struct pci_region *reg = &hose->regions[i];
    
	// 	if (reg->flags != PCI_REGION_MEM)
	// 		continue;
    
	// 	if (num_out_wins >= BRCM_NUM_PCIE_OUT_WINS)
	// 		return false;
    
	// 	brcm_pcie_set_outbound_win(pcie, num_out_wins, reg->phys_start,
	// 				   reg->bus_start, reg->size);
    
	// 	num_out_wins++;
	// }
	
	while (true){ }
    
	// /*
	//  * For config space accesses on the RC, show the right class for
	//  * a PCIe-PCIe bridge (the default setting is to be EP mode).
	//  */
	// clrsetbits_le32(base + PCIE_RC_CFG_PRIV1_ID_VAL3,
	// 		PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK, 0x060400);
    
	// if (pcie->ssc) {
	// 	ret = brcm_pcie_set_ssc(pcie->base);
	// 	if (!ret)
	// 		ssc_good = true;
	// 	else
	// 		printf("PCIe BRCM: failed attempt to enter SSC mode\n");
	// }
    
	// lnksta = readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA);
	// cls = lnksta & PCI_EXP_LNKSTA_CLS;
	// nlw = (lnksta & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;
    
	// printf("PCIe BRCM: link up, %s Gbps x%u %s\n", link_speed_to_str(cls),
	//        nlw, ssc_good ? "(SSC)" : "(!SSC)");
    
	// /* PCIe->SCB endian mode for BAR */
	// clrsetbits_le32(base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1,
	// 		PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK,
	// 		VENDOR_SPECIFIC_REG1_LITTLE_ENDIAN);
    
	// /*
	//  * We used to enable the CLKREQ# input here, but a few PCIe cards don't
	//  * attach anything to the CLKREQ# line, so we shouldn't assume that
	//  * it's connected and working. The controller does allow detecting
	//  * whether the port on the other side of our link is/was driving this
	//  * signal, so we could check before we assume. But because this signal
	//  * is for power management, which doesn't make sense in a bootloader,
	//  * let's instead just unadvertise ASPM support.
	//  */
	// clrbits_le32(base + PCIE_RC_CFG_PRIV1_LINK_CAPABILITY,
	// 	     LINK_CAPABILITY_ASPM_SUPPORT_MASK);
    
    while (true);
    
    return true;
}
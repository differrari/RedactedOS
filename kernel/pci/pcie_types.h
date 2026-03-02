#pragma once

//Adapted from https://github.com/rsta2/circle/blob/master/lib/bcmpciehostbridge.cpp

#define PCI_DEVFN(slot, func)	((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn)		(((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)		((devfn) & 0x07)

#define PCIE_BUSNUM_SHIFT		20
#define PCIE_SLOT_SHIFT			15
#define PCIE_FUNC_SHIFT			12

#define PCIE_EXT_CFG_INDEX		0x9000
#define PCIE_EXT_CFG_DATA		0x8000

#define PCI_CLASS_REVISION	0x08	/* High 24 bits are class, low 8 revision */
#define PCI_REVISION_ID		0x08	/* Revision ID */
#define PCI_CLASS_PROG		0x09	/* Reg. Level Programming Interface */
#define PCI_CLASS_DEVICE	0x0a	/* Device class */

#define PCI_CACHE_LINE_SIZE	0x0c	/* 8 bits */
#define PCI_LATENCY_TIMER	0x0d	/* 8 bits */
#define PCI_HEADER_TYPE		0x0e	/* 8 bits */
#define  PCI_HEADER_TYPE_NORMAL		0
#define  PCI_HEADER_TYPE_BRIDGE		1
#define  PCI_HEADER_TYPE_CARDBUS	2

#define PCI_PRIMARY_BUS		0x18	/* Primary bus number */
#define PCI_SECONDARY_BUS	0x19	/* Secondary bus number */
#define PCI_SUBORDINATE_BUS	0x1a	/* Highest bus number behind the bridge */
#define PCI_SEC_LATENCY_TIMER	0x1b	/* Latency timer for secondary interface */
#define PCI_IO_BASE		0x1c	/* I/O range behind the bridge */
#define PCI_IO_LIMIT		0x1d
#define  PCI_IO_RANGE_TYPE_MASK	0x0fUL	/* I/O bridging type */
#define  PCI_IO_RANGE_TYPE_16	0x00
#define  PCI_IO_RANGE_TYPE_32	0x01
#define  PCI_IO_RANGE_MASK	(~0x0fUL) /* Standard 4K I/O windows */
#define  PCI_IO_1K_RANGE_MASK	(~0x03UL) /* Intel 1K I/O windows */
#define PCI_SEC_STATUS		0x1e	/* Secondary status register, only bit 14 used */
#define PCI_MEMORY_BASE		0x20	/* Memory range behind */
#define PCI_MEMORY_LIMIT	0x22
#define  PCI_MEMORY_RANGE_TYPE_MASK 0x0fUL
#define  PCI_MEMORY_RANGE_MASK	(~0x0fUL)
#define PCI_PREF_MEMORY_BASE	0x24	/* Prefetchable memory range behind */
#define PCI_PREF_MEMORY_LIMIT	0x26
#define  PCI_PREF_RANGE_TYPE_MASK 0x0fUL
#define  PCI_PREF_RANGE_TYPE_32	0x00
#define  PCI_PREF_RANGE_TYPE_64	0x01
#define  PCI_PREF_RANGE_MASK	(~0x0fUL)
#define PCI_PREF_BASE_UPPER32	0x28	/* Upper half of prefetchable memory range */
#define PCI_PREF_LIMIT_UPPER32	0x2c
#define PCI_IO_BASE_UPPER16	0x30	/* Upper half of I/O addresses */
#define PCI_IO_LIMIT_UPPER16	0x32
/* 0x34 same as for htype 0 */
/* 0x35-0x3b is reserved */
#define PCI_ROM_ADDRESS1	0x38	/* Same as PCI_ROM_ADDRESS, but for htype 1 */
/* 0x3c-0x3d are same as for htype 0 */
#define PCI_BRIDGE_CONTROL	0x3e
#define  PCI_BRIDGE_CTL_PARITY	0x01	/* Enable parity detection on secondary interface */
#define  PCI_BRIDGE_CTL_SERR	0x02	/* The same for SERR forwarding */
#define  PCI_BRIDGE_CTL_ISA	0x04	/* Enable ISA mode */
#define  PCI_BRIDGE_CTL_VGA	0x08	/* Forward VGA addresses */
#define  PCI_BRIDGE_CTL_MASTER_ABORT	0x20  /* Report master aborts */
#define  PCI_BRIDGE_CTL_BUS_RESET	0x40	/* Secondary bus reset */
#define  PCI_BRIDGE_CTL_FAST_BACK	0x80	/* Fast Back2Back enabled on secondary interface */

#define PCI_STD_HEADER_SIZEOF	64
#define PCI_VENDOR_ID		0x00	/* 16 bits */
#define PCI_DEVICE_ID		0x02	/* 16 bits */
#define PCI_COMMAND		0x04	/* 16 bits */
#define  PCI_COMMAND_IO		0x1	/* Enable response in I/O space */
#define  PCI_COMMAND_MEMORY	0x2	/* Enable response in Memory space */
#define  PCI_COMMAND_MASTER	0x4	/* Enable bus mastering */
#define  PCI_COMMAND_SPECIAL	0x8	/* Enable response to special cycles */
#define  PCI_COMMAND_INVALIDATE	0x10	/* Use memory write and invalidate */
#define  PCI_COMMAND_VGA_PALETTE 0x20	/* Enable palette snooping */
#define  PCI_COMMAND_PARITY	0x40	/* Enable parity checking */
#define  PCI_COMMAND_WAIT	0x80	/* Enable address/data stepping */
#define  PCI_COMMAND_SERR	0x100	/* Enable SERR */
#define  PCI_COMMAND_FAST_BACK	0x200	/* Enable back-to-back writes */
#define  PCI_COMMAND_INTX_DISABLE 0x400 /* INTx Emulation Disable */

#define PCI_CAP_LIST_ID		0	/* Capability ID */
#define  PCI_CAP_ID_PM		0x01	/* Power Management */
#define  PCI_CAP_ID_AGP		0x02	/* Accelerated Graphics Port */
#define  PCI_CAP_ID_VPD		0x03	/* Vital Product Data */
#define  PCI_CAP_ID_SLOTID	0x04	/* Slot Identification */
#define  PCI_CAP_ID_MSI		0x05	/* Message Signalled Interrupts */
#define  PCI_CAP_ID_CHSWP	0x06	/* CompactPCI HotSwap */
#define  PCI_CAP_ID_PCIX	0x07	/* PCI-X */
#define  PCI_CAP_ID_HT		0x08	/* HyperTransport */
#define  PCI_CAP_ID_VNDR	0x09	/* Vendor-Specific */
#define  PCI_CAP_ID_DBG		0x0A	/* Debug port */
#define  PCI_CAP_ID_CCRC	0x0B	/* CompactPCI Central Resource Control */
#define  PCI_CAP_ID_SHPC	0x0C	/* PCI Standard Hot-Plug Controller */
#define  PCI_CAP_ID_SSVID	0x0D	/* Bridge subsystem vendor/device ID */
#define  PCI_CAP_ID_AGP3	0x0E	/* AGP Target PCI-PCI bridge */
#define  PCI_CAP_ID_SECDEV	0x0F	/* Secure Device */
#define  PCI_CAP_ID_EXP		0x10	/* PCI Express */
#define  PCI_CAP_ID_MSIX	0x11	/* MSI-X */
#define  PCI_CAP_ID_SATA	0x12	/* SATA Data/Index Conf. */
#define  PCI_CAP_ID_AF		0x13	/* PCI Advanced Features */
#define  PCI_CAP_ID_EA		0x14	/* PCI Enhanced Allocation */
#define  PCI_CAP_ID_MAX		PCI_CAP_ID_EA
#define PCI_CAP_LIST_NEXT	1	/* Next capability in the list */
#define PCI_CAP_FLAGS		2	/* Capability defined flags (16 bits) */
#define PCI_CAP_SIZEOF		4

#define PCI_EXP_RTCTL		28	/* Root Control */
#define  PCI_EXP_RTCTL_SECEE	0x0001	/* System Error on Correctable Error */
#define  PCI_EXP_RTCTL_SENFEE	0x0002	/* System Error on Non-Fatal Error */
#define  PCI_EXP_RTCTL_SEFEE	0x0004	/* System Error on Fatal Error */
#define  PCI_EXP_RTCTL_PMEIE	0x0008	/* PME Interrupt Enable */
#define  PCI_EXP_RTCTL_CRSSVE	0x0010	/* CRS Software Visibility Enable */

#define MEM_PCIE_RANGE_START    0x600000000ULL
#define MEM_PCIE_RANGE_SIZE		0x4000000ULL

#define MEM_PCIE_RANGE_PCIE_START	    0xF8000000ULL		
#define MEM_PCIE_RANGE_START_VIRTUAL	0xFA000000UL
#define MEM_PCIE_RANGE_END_VIRTUAL	    (MEM_PCIE_RANGE_START_VIRTUAL + 2 * MEGABYTE - 1ULL)

#define MEM_PCIE_DMA_RANGE_START	0ULL
#define MEM_PCIE_DMA_RANGE_SIZE		0x100000000ULL
#define MEM_PCIE_DMA_RANGE_PCIE_START	0ULL			

#define PCIE_MSI_INTR2_CLR				0x4508
#define PCIE_MSI_INTR2_MASK_SET         0x4510

#define  STATUS_PCIE_DL_ACTIVE_MASK     0x20
#define  STATUS_PCIE_DL_ACTIVE_SHIFT	5
#define  STATUS_PCIE_PHYLINKUP_MASK		0x10
#define  STATUS_PCIE_PHYLINKUP_SHIFT	4

#define PCIE_STATUS                                             0x4068

#define PCIE_MISC_MISC_CTRL                                     0x4008

#define PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK			0x300000
#define PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_SHIFT		0x14
#define PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK			    0xf8000000
#define PCIE_MISC_MISC_CTRL_SCB0_SIZE_SHIFT			    0x1b
#define PCIE_MISC_MISC_CTRL_SCB1_SIZE_MASK			    0x7c00000
#define PCIE_MISC_MISC_CTRL_SCB1_SIZE_SHIFT			    0x16
#define PCIE_MISC_MISC_CTRL_SCB2_SIZE_MASK			    0x1f
#define PCIE_MISC_MISC_CTRL_SCB2_SIZE_SHIFT			    0x0
#define PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK		0x2000
#define PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_SHIFT		0xd

#define PCIE_MISC_HARD_PCIE_HARD_DEBUG                          0x4304
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK         0x08000000
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_SHIFT        0x1b
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_L1SS_ENABLE_MASK  0x200000
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK 0x2
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_SHIFT 0x1

#define PCIE_RGR1_SW_INIT_1                                    0x9210
#define PCIE_RGR1_SW_INIT_1_INIT_MASK                           0x2
#define PCIE_RGR1_SW_INIT_1_INIT_SHIFT                          0x1
#define PCIE_RGR1_SW_INIT_1_PERST_MASK                          0x1
#define PCIE_RGR1_SW_INIT_1_PERST_SHIFT                         0x0

#define BURST_SIZE_128			0
#define BURST_SIZE_256			1
#define BURST_SIZE_512			2

#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402c
#define PCIE_MISC_RC_BAR1_CONFIG_HI			0x4030
#define PCIE_MISC_RC_BAR2_CONFIG_LO			0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI			0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO			0x403c
#define PCIE_MISC_MSI_BAR_CONFIG_LO			0x4044
#define PCIE_MISC_MSI_BAR_CONFIG_HI			0x4048

#define PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK			0x1f
#define PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_SHIFT			0x0
#define PCIE_MISC_RC_BAR2_CONFIG_LO_SIZE_MASK			0x1f
#define PCIE_MISC_RC_BAR2_CONFIG_LO_SIZE_SHIFT			0x0
#define PCIE_MISC_RC_BAR3_CONFIG_LO_SIZE_MASK			0x1f
#define PCIE_MISC_RC_BAR3_CONFIG_LO_SIZE_SHIFT			0x0

#define PCIE_INTR2_CPU_BASE				0x4300
#define STATUS				0x0
#define SET				0x4
#define CLR				0x8


#define PCI_EXP_LNKCAP_SLS	        0x0000000f
#define PCI_EXP_LNKCAP_SLS_2_5GB    0x00000001
#define PCI_EXP_LNKCAP_SLS_5_0GB    0x00000002
#define PCI_EXP_LNKCAP_SLS_8_0GB    0x00000003
#define PCI_EXP_LNKCAP_SLS_16_0GB   0x00000004
#define BRCM_PCIE_CAP_REGS			0x00ac
#define PCIE_GEN	                2
#define PCI_EXP_LNKCAP		        12
#define PCI_EXP_LNKCTL2		        48
#define PCI_EXP_LNKCTL2_TLS		    0x000
#define PCI_EXP_LNKCTL2_TLS_2_5GT	0x0001
#define PCI_EXP_LNKCTL2_TLS_5_0GT	0x0002
#define PCI_EXP_LNKCTL2_TLS_8_0GT	0x0003
#define PCI_EXP_LNKCTL2_TLS_16_0GT	0x0004

#define PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK			0x1000
#define PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_SHIFT			0xc

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK	0xfff00000
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_SHIFT	0x14
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK	0xfff0
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_SHIFT	0x4
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_NUM_MASK_BITS	0xc
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK		0xff
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_SHIFT	0x0
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK	0xff
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_SHIFT	0x0

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT	0x4070
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI		0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI		0x4084

#define PCI_EXP_LNKSTA		18	/* Link Status */
#define  PCI_EXP_LNKSTA_CLS	0x000f	/* Current Link Speed */
#define  PCI_EXP_LNKSTA_CLS_2_5GB 0x0001 /* Current Link Speed 2.5GT/s */
#define  PCI_EXP_LNKSTA_CLS_5_0GB 0x0002 /* Current Link Speed 5.0GT/s */
#define  PCI_EXP_LNKSTA_CLS_8_0GB 0x0003 /* Current Link Speed 8.0GT/s */
#define  PCI_EXP_LNKSTA_CLS_16_0GB 0x0004 /* Current Link Speed 16.0GT/s */
#define  PCI_EXP_LNKSTA_NLW	0x03f0	/* Negotiated Link Width */
#define  PCI_EXP_LNKSTA_NLW_X1	0x0010	/* Current Link Width x1 */
#define  PCI_EXP_LNKSTA_NLW_X2	0x0020	/* Current Link Width x2 */
#define  PCI_EXP_LNKSTA_NLW_X4	0x0040	/* Current Link Width x4 */
#define  PCI_EXP_LNKSTA_NLW_X8	0x0080	/* Current Link Width x8 */
#define  PCI_EXP_LNKSTA_NLW_SHIFT 4	/* start of NLW mask in link status */
#define  PCI_EXP_LNKSTA_LT	0x0800	/* Link Training */
#define  PCI_EXP_LNKSTA_SLC	0x1000	/* Slot Clock Configuration */
#define  PCI_EXP_LNKSTA_DLLLA	0x2000	/* Data Link Layer Link Active */
#define  PCI_EXP_LNKSTA_LBMS	0x4000	/* Link Bandwidth Management Status */
#define  PCI_EXP_LNKSTA_LABS	0x8000	/* Link Autonomous Bandwidth Status */

#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1		0x0188
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK	0xc
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_SHIFT	0x2

#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043c
#define PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK		0xffffff
#define PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_SHIFT		0x0

#define PCIE_MISC_PCIE_STATUS				0x4068

#define PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK			0x80
#define PCIE_MISC_PCIE_STATUS_PCIE_PORT_SHIFT			0x7

#define lo32(addr) addr & UINT32_MAX
#define hi32(addr) (addr >> 32) & UINT32_MAX
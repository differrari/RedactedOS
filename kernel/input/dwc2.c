#include "dwc2.h"
#include "async.h"
#include "console/kio.h"

#define DWC2_BASE 0xFE980000

typedef struct {
    uint32_t gotgctl;
    uint32_t gotgint;
    uint32_t gahbcfg;
    uint32_t gusbcfg;
    uint32_t grstctl;
    uint32_t gintsts;
    uint32_t gintmsk;
    uint32_t grxstsr;
    uint32_t grxstsp;
    uint32_t grxfsiz;
    uint32_t gnptxfsiz;
    uint32_t gnptxsts;
} dwc2_regs;

typedef struct {
    uint32_t hstcfg;
    uint32_t frminterval;
    uint32_t frmnum;
    uint32_t rsvd;
    uint32_t ptx_fifo;
    uint32_t allchan_int;
    uint32_t allchan_int_mask;
    uint32_t frmlst_base;
    uint32_t rsvd2[8];
    uint32_t port;
} dwc2_host;

bool dwc2_init() {

    dwc2_regs *dwc2 = (dwc2_regs*)DWC2_BASE;
    dwc2_host *host = (dwc2_host*)(DWC2_BASE + 0x400);

    *(uint32_t*)(DWC2_BASE + 0xE00) = 0;//Power reset

    dwc2->grstctl |= 1;
    if (!wait(&dwc2->grstctl, 1, false, 2000)){
        kprintf("[DWC2] Failed to reset");
        return false;
    }

    dwc2->gusbcfg &= ~(1 << 30);//Device mode disable
    dwc2->gusbcfg |= (1 << 29);//Host mode enable

    delay(200);

    dwc2->gintmsk = 0xffffffff;

    host->port |= (1 << 12);
    
    if (!wait(&host->port, 1, true, 2000)){
        kprintf("[DWC2] No device connected %x",host->port);
        return false;
    }

    delay(100);

    host->port &= ~0b101110;
    host->port |= (1 << 8);

    delay(50);

    host->port &= ~0b101110;
    host->port &= ~(1 << 8);

    kprintf("Port reset %x",host->port);

    return true;
}
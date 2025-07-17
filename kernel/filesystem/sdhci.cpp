#include "sdhci.hpp"
#include "console/kio.h"
#include "hw/hw.h"
#include "mailbox/mailbox.h"
#include "syscalls/syscalls.h"
#include "memory/mmu.h"
#include "std/memfunctions.h"
#include "async.h"

#define CMD_INDEX(x)   ((x) << 8)
#define RESP_TYPE(x)   (x)
#define CRC_ENABLE     (1 << 3)
#define IDX_ENABLE     (1 << 4)
#define IS_DATA   (1 << 5)

#define SEND_CID (CMD_INDEX(2) | RESP_TYPE(1) | CRC_ENABLE)
#define IF_COND (CMD_INDEX(8) | RESP_TYPE(2) | CRC_ENABLE | IDX_ENABLE)
#define RCA (CMD_INDEX(3) | RESP_TYPE(2) | CRC_ENABLE)
#define APP (CMD_INDEX(55) | RESP_TYPE(2) | CRC_ENABLE | IDX_ENABLE)
#define SELECT_CARD (CMD_INDEX(7) | RESP_TYPE(3) | CRC_ENABLE | IDX_ENABLE)
#define OCR (CMD_INDEX(41) | RESP_TYPE(2))
#define READ_ONE (CMD_INDEX(17) | RESP_TYPE(2) | CRC_ENABLE | IS_DATA)
#define READ_MULTIPLE (CMD_INDEX(18) | RESP_TYPE(2) | CRC_ENABLE | IS_DATA)
#define STOP_TRANSMISSION (CMD_INDEX(12) | RESP_TYPE(2) | CRC_ENABLE)
#define SET_BLOCKLEN (CMD_INDEX(16) | RESP_TYPE(2) | CRC_ENABLE)

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            uint64_t _args[] = { __VA_ARGS__ }; \
            kprintf_args_raw((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
        }\
    })

void SDHCI::enable_verbose(){
    verbose = true;
}

uint32_t SDHCI::clock_divider(uint32_t target_rate) {
    uint32_t target_div = 1;

    if (target_rate <= clock_rate) {
        target_div = clock_rate / target_rate;

        if (clock_rate % target_rate) {
            target_div = 0;
        }
    }

    int div = -1;
    for (int fb = 31; fb >= 0; fb--) {
        uint32_t bt = (1 << fb);

        if (target_div & bt) {
            div = fb;
            target_div &= ~(bt);

            if (target_div) {
                div++;
            }

            break;
        }
    }

    if (div == -1)
        div = 31;

    if (div >= 32)
        div = 31;

    if (div != 0)
        div = (1 << (div - 1));

    if (div >= 0x400)
        div = 0x3FF;

    uint32_t freq = div & 0xff;
    uint32_t hi = (div >> 8) & 0x3;
    uint32_t ret = (freq << 8) | (hi << 6) | (0 << 5);

    return ret;
}

bool SDHCI::switch_clock_rate(uint32_t target_rate) {
    uint32_t div = clock_divider(target_rate);

    if (!wait(&regs->status,0b11, false, 2000)) return false;

    regs->ctrl1 &= ~(1 << 2);

    delay(3);

    regs->ctrl1 = (regs->ctrl1 | div) & ~0xFFE0;

    delay(3);

    regs->ctrl1 |= (1 << 2);

    delay(3);

    return true;
}

volatile uint32_t mbox[8] __attribute__((aligned(16))) = {
    32,
    0,
    MBOX_CLKRATE_TAG, 8, 0, 1, 0,//Request clock rate for 1 (EMMC)
    0
};

bool SDHCI::setup_clock(){
    regs->ctrl2 = 0;

    if (!mailbox_call(mbox,8)) return false;

    clock_rate = mbox[6];

    regs->ctrl1 |= 1;
    regs->ctrl1 |= clock_divider(400000);
    regs->ctrl1 &= ~(0xf << 16);
    regs->ctrl1 |= (11 << 16);
    regs->ctrl1 |= (1 << 2);

    if (!wait(&regs->ctrl1, (1 << 1), true, 2000)) return false;

    delay(30);

    regs->ctrl1 |= 4;

    delay(30);

    return true;
}

bool SDHCI::init() {
    if (!SDHCI_BASE) return false;

    register_device_memory(SDHCI_BASE, SDHCI_BASE);

    regs = (sdhci_regs*)SDHCI_BASE;

    regs->ctrl1 |= (1 << 24);//Reset
    if (!wait(&regs->ctrl1,(1 << 24), false, 2000)) return false;

    if (RPI_BOARD >= 4){
        regs->ctrl0 |= 0x0F << 8;//VDD1 bus power
        delay(3);
    }

    kprintfv("[SDHCI] Controller reset");

    setup_clock();

    kprintf("[SDHCI] Controller ready CTL0 %x",regs->ctrl0);

    regs->interrupt = 0;
    regs->irpt_en = 0xFFFFFFFF;
    regs->irpt_mask = 0xFFFFFFFF;

    delay(203);

    if (!issue_command(0, 0)){
        kprintf("[SDHCI error] Failed setting to idle");
        return false;
    }
    kprintfv("[SDHCI] Idle");

    switch_clock_rate(25000000);

    bool v2_card = 0;  
    if (!issue_command(IF_COND, 0x1AA)){ 
        if (!(regs->interrupt & 0x10000)){
            kprintf("[SDHCI error] IFCOND error");
            return false;
        }
        kprintfv("[SDHCI] Timeout on IFCOND. Defaulting to V1");
    } else {
        if ((regs->resp0 & 0xFFF) != 0x1AA) {
            kprintf("[SDHCI error] IFCOND pattern mismatch");
            return false;
        }
        v2_card = true;
    }
    
    kprintfv("[SDHCI] V2 = %i",v2_card);

    while (true){
        if (!issue_app_command(OCR, 0xFF8000 | (v2_card << 30))){
            kprintf("[SDHCI error] Get OCR Failed");
            return false;
        }
        if (regs->resp0 & (1 << 31)) break;
        else delay(500);
    }

    kprintfv("[SDHCI] OCR %x",regs->resp0);

    delay(1000);

    if (!issue_command(SEND_CID, 0)){ 
        kprintf("[SDHCI error] Failed requesting card id");
        return false;
    }
    kprintf("[SDHCI] Card ID: %x.%x.%x.%x",regs->resp0, regs->resp1,regs->resp2,regs->resp3);
    if (!issue_command(RCA, 0)){
        kprintf("[SDHCI error] Failed to send relative address");
        return false;
    }

    rca = (regs->resp0 >> 16) & 0xFFFF;
    kprintfv("[SDHCI] RCA %x",rca);

    if (!issue_command(SELECT_CARD, rca << 16)){
        kprintf("[SDHCI error] Failed to select card");
        return false;
    }

    if (!issue_command(SET_BLOCKLEN, 512)){
        kprintf("[SDHCI error] Failed to set block length");
        return false;
    }

    return true;
}

bool SDHCI::issue_app_command(uint32_t cmd, uint32_t arg, uint32_t flags) {
    if (issue_command(APP, 0, 1 << 3))
        return issue_command(cmd, arg);

    return false;
}

bool SDHCI::issue_command(uint32_t cmd, uint32_t arg, uint32_t flags) {

    if (!wait(&regs->status, 0b11, false, 2000)) {
        kprintf("[SDHCI error] Timeout waiting for CMD/DAT inhibit");
        return false;
    }

    regs->interrupt = 0xFFFFFFFF;

    regs->arg1 = arg;
    regs->cmd_transfmode = (cmd << 16) | flags;

    kprintfv("[SDHCI] Sent command %b.",(cmd << 16) | flags);

    if (!wait(&regs->interrupt,0x1, true, 2000)) { 
        kprintf("[SDHCI warning] Issue command timeout %x",regs->interrupt); 
        return false; 
    }

    kprintfv("[SDHCI] Command finished %x",regs->interrupt);
    
    if (regs->interrupt & 0x8000) return false;//Error
    if (!(regs->interrupt & 0x0001)) return false;//No finish

    return true;
}

bool SDHCI::read(void *buffer, uint32_t sector, uint32_t count){
    bool multiple = count > 1;
    regs->blksize_count = (count << 16) | 512;
    uint32_t command = multiple ? READ_MULTIPLE : READ_ONE;
    uint32_t flags = multiple ? 0b110110 : 0b010000;
    for (int i = 5; i >= 0; i--){
        //TODO: Byte addressing works here, instead of block addressing. Not sure that's normal or if it's card-specific
        if (issue_command(command, sector * 512, flags)) break;
        if (i == 0) { kprintf("[SDHCI error] read request timeout"); return false; }
        delay(500);
    }

    uint32_t* dest = (uint32_t*)buffer;
    for (uint32_t i = 0; i < count; i++) {
        if (!wait(&regs->interrupt,(1 << 5), true, 2000)){
            kprintf("[SDHCI error] Read operation timed out on block %i %x",i,regs->interrupt);
            memset(buffer,0,count * 128);
            return false;
        }

        regs->interrupt = 0xFFFFFFFF;
        
        for (int j = 0; j < 128; j++)
            dest[(i * 128) + j] = regs->data;
        
        if (regs->interrupt & (1 << 1))
            break;
    }

    if (multiple) {
        if (!wait(&regs->interrupt, (1 << 1), true, 2000)) {
            kprintf("[SDHCI error] Timed out waiting for DATA_DONE");
            return false;
        }
        regs->interrupt = 0xFFFFFFFF;
    }

    return true;
}

//TODO: Write (CMD24 block, CMD25 multiple). Read can be generalized.
//TODO: DMA
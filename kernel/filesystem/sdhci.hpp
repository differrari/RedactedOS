#pragma once

#include "types.h"

typedef struct sdhci_regs {
    uint32_t arg2;
    uint32_t blksize_count;
    uint32_t arg1;
    uint32_t cmd_transfmode;
    uint32_t resp0;
    uint32_t resp1;
    uint32_t resp2;
    uint32_t resp3;
    uint32_t data;
    uint32_t status;
    uint32_t ctrl0;
    uint32_t ctrl1;
    uint32_t interrupt;
    uint32_t irpt_mask;
    uint32_t irpt_en;
    uint32_t ctrl2;
    uint32_t __reserved[0x2F];
    uint32_t slotisr_ver;
} sdhci_regs;

class SDHCI {
public:
    bool init();
    void write(void *buffer, uint32_t sector, uint32_t count);
    bool read(void *buffer, uint32_t sector, uint32_t count);
private:
    sdhci_regs* regs;
    bool issue_command(uint32_t cmd_index, uint32_t arg, uint32_t flags = 0);
    bool issue_app_command(uint32_t cmd, uint32_t arg, uint32_t flags = 0);
    bool setup_clock();
    uint32_t clock_divider(uint32_t target_rate);
    bool switch_clock_rate(uint32_t target_rate);
    void dump();
    bool wait(uint32_t *reg, uint32_t expected_value, bool match = true, uint32_t timeout = 2000);
    uint32_t clock_rate;
    uint32_t rca;
};
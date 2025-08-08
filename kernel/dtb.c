#include "dtb.h"
#include "console/kio.h"
#include "std/string.h"
#include "memory/talloc.h"

#define DTB_ADDR 0x40000000UL
#define FDT_MAGIC 0xD00DFEED

#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static struct fdt_header *hdr;

bool dtb_get_header(){
    if (!hdr)
        hdr = (struct fdt_header *)DTB_ADDR;
    if (__builtin_bswap32(hdr->magic) != FDT_MAGIC) return false;
    return true;
}

bool dtb_addresses(uint64_t *start, uint64_t *size){
    if (!dtb_get_header()) return false;
    *start = (uint64_t)DTB_ADDR;
    *size = __builtin_bswap32(hdr->totalsize);
    return true;
}

void dtb_debug_print_all() {
    if (!dtb_get_header()) return;

    uint32_t *p = (uint32_t *)(DTB_ADDR + __builtin_bswap32(hdr->off_dt_struct));

    while (1) {
        uint32_t token = __builtin_bswap32(*p++);
        if (token == FDT_END) break;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            uint32_t skip = 0;
            while (((char *)p)[skip]) skip++;
            skip = (skip + 4) & ~3;
            p += skip / 4;
            kprintf(name);
        } 
    }
}

bool dtb_scan(const char *search_name, dtb_node_handler handler, dtb_match_t *match) {
    if (!dtb_get_header()) return false;

    uint32_t *p = (uint32_t *)(DTB_ADDR + __builtin_bswap32(hdr->off_dt_struct));
    const char *strings = (const char *)(DTB_ADDR + __builtin_bswap32(hdr->off_dt_strings));
    int depth = 0;
    bool active = 0;

    while (1) {
        uint32_t token = __builtin_bswap32(*p++);
        if (token == FDT_END) break;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            uint32_t skip = 0;
            while (((char *)p)[skip]) skip++;
            skip = (skip + 4) & ~3;
            p += skip / 4;
            depth++;
            active = strcont(name, search_name);
        } else if (token == FDT_PROP && active) {
            uint32_t len = __builtin_bswap32(*p++);
            uint32_t nameoff = __builtin_bswap32(*p++);
            const char *propname = strings + nameoff;
            const void *prop = p;

            handler(propname, prop, len, match);
        
            p += (len + 3) / 4;
        } else if (token == FDT_END_NODE) {
            depth--;
            if (active && match->found)
                return true;
            active = 0;
            match->compatible = 0;
            match->reg_base = 0;
            match->reg_size = 0;
            match->irq = 0;
            match->found = 0;
        }
    }
    return false;
}
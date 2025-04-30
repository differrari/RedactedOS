#include "dtb.h"
#include "console/kio.h"
#include "kstring.h"
#include "ram_e.h"

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

static inline uint32_t bswap32(uint32_t x) {
    return __builtin_bswap32(x);
}

static inline uint64_t bswap64(uint64_t x) {
    return ((uint64_t)bswap32(x & 0xFFFFFFFF) << 32) | bswap32(x >> 32);
}


typedef int (*dtb_node_handler)(const char *name, const char *propname, const void *prop, uint32_t len, dtb_match_t *out);

int dtb_scan(const char *search_name, dtb_node_handler handler, dtb_match_t *match) {
    struct fdt_header *hdr = (struct fdt_header *)DTB_ADDR;
    if (bswap32(hdr->magic) != FDT_MAGIC) return 0;

    uint32_t *p = (uint32_t *)(DTB_ADDR + bswap32(hdr->off_dt_struct));
    const char *strings = (const char *)(DTB_ADDR + bswap32(hdr->off_dt_strings));
    int depth = 0;
    bool active = 0;

    while (1) {
        uint32_t token = bswap32(*p++);
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
            uint32_t len = bswap32(*p++);
            uint32_t nameoff = bswap32(*p++);
            const char *propname = strings + nameoff;
            const void *prop = p;
            if (handler(NULL, propname, prop, len, match)) return 1;
            p += (len + 3) / 4;
        } else if (token == FDT_END_NODE) {
            depth--;
            active = 0;
        }
    }
    return 0;
}

int handle_mem_node(const char *name, const char *propname, const void *prop, uint32_t len, dtb_match_t *match) {
    if (strcmp(propname, "reg") == 0 && len >= 16) {
        uint32_t *p = (uint32_t *)prop;
        match->reg_base = ((uint64_t)bswap32(p[0]) << 32) | bswap32(p[1]);
        match->reg_size = ((uint64_t)bswap32(p[2]) << 32) | bswap32(p[3]);
        return 1;
    }
    return 0;
}

int get_memory_region(uint64_t *out_base, uint64_t *out_size) {
    dtb_match_t match = {0};
    if (dtb_scan("memory",handle_mem_node, &match)) {
        *out_base = match.reg_base;
        *out_size = match.reg_size;
        return 1;
    }
    return 0;
}

int handle_virtio_node(const char *name, const char *propname, const void *prop, uint32_t len, dtb_match_t *match) {
    if (strcmp(propname, "compatible") == 0 && len >= 12 && memcmp(prop, "virtio,mmio", 11) == 0) {
        match->found = 1;
    } else if (match->found && strcmp(propname, "reg") == 0 && len >= 16) {
        uint32_t *p = (uint32_t *)prop;
        match->reg_base = ((uint64_t)bswap32(p[0]) << 32) | bswap32(p[1]);
        match->reg_size = ((uint64_t)bswap32(p[2]) << 32) | bswap32(p[3]);
    } else if (match->found && strcmp(propname, "interrupts") == 0 && len >= 4) {
        match->irq = bswap32(*(uint32_t *)prop);
        return 1;
    }
    return 0;
}

int find_virtio_blk(uint64_t *out_base, uint64_t *out_size, uint32_t *out_irq) {
    dtb_match_t match = {0};
    if (dtb_scan("virtio",handle_virtio_node, &match)) {
        *out_base = match.reg_base;
        *out_size = match.reg_size;
        *out_irq = match.irq;
        return 1;
    }
    return 0;
}
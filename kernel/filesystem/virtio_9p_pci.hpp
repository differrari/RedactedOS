#pragma once

#include "fsdriver.hpp"
#include "virtio/virtio_pci.h"
#include "data_struct/hashmap.h"

typedef struct p9_packet_header {
    uint32_t size;
    uint8_t id;
    uint16_t tag;
}__attribute__((packed)) p9_packet_header;
static_assert(sizeof(p9_packet_header) == 7, "Wrong size of packet header");

class Virtio9PDriver : public FSDriver {
public:
    bool init(uint32_t partition_sector) override;
    FS_RESULT open_file(const char* path, file* descriptor) override;
    size_t read_file(file *descriptor, void* buf, size_t size) override;
    size_t list_contents(const char *path, void* buf, size_t size, uint64_t *offset) override;
    void close_file(file* descriptor) override;
private:
    virtio_device np_dev;
    size_t choose_version();
    uint32_t open(uint32_t fid);
    uint32_t attach();
    size_t list_contents(uint32_t fid, void *buf, size_t size, uint64_t *offset);
    uint32_t walk_dir(uint32_t fid, char *path);
    uint64_t read(uint32_t fid, uint64_t offset, void* file);
    uint64_t get_attribute(uint32_t fid, uint64_t mask);
    void p9_max_tag(p9_packet_header* header);
    void p9_inc_tag(p9_packet_header* header);
    size_t max_msize;
    uint32_t vfid;
    uint32_t mid;

    uint32_t root;

    chashmap_t *open_files;
};
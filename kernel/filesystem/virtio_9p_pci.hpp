#pragma once

#include "fsdriver.hpp"
#include "virtio/virtio_pci.h"
#include "data/struct/hashmap.h"
#include "p9_helper.h"

class Virtio9PDriver : public FSDriver {
public:
    bool init(uint32_t partition_sector) override;
    FS_RESULT open_file(const char* path, file* descriptor) override;
    size_t read_file(file *descriptor, void* buf, size_t size) override;
    size_t write_file(file *descriptor, const char* buf, size_t size) override;
    size_t list_contents(const char *path, void* buf, size_t size, uint64_t *offset) override;
    void close_file(file* descriptor) override;
    bool stat(const char *path, fs_stat *out_stat) override;
    bool truncate(file *descriptor, size_t size) override;
private:
    virtio_device np_dev = {};
    size_t choose_version();
    uint32_t open(uint32_t fid);
    bool sync_file(module_file *mfile);
    uint32_t attach();
    size_t list_contents(uint32_t fid, void *buf, size_t size, uint64_t *offset);
    uint32_t walk_dir(uint32_t fid, char *path);
    uint64_t read(uint32_t fid, uint64_t offset, void* file);
    size_t write(u32 fid, u64 offset, size_t amount, const char* buf);
    r_getattr* get_attribute(uint32_t fid, uint64_t mask);
    bool set_attribute(u32 fid, u64 mask, u64 value);
    bool clunk(virtio_device *dev, uint32_t fid);
    size_t max_msize = 0;

    uint32_t root = 0;

    chashmap_t *open_files = nullptr;
};
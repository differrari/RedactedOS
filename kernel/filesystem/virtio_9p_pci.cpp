#include "virtio_9p_pci.hpp"
#include "virtio/virtio_pci.h"
#include "console/kio.h"
#include "pci.h"
#include "memory/page_allocator.h"
#include "exceptions/exception_handler.h"
#include "exceptions/irq.h"
#include "std/memory.h"
#include "std/memory_access.h"
#include "p9_helper.h"

#define VIRTIO_9P_ID 0x1009

#define INVALID_FID UINT32_MAX

bool Virtio9PDriver::init(uint32_t partition_sector){
    uint64_t addr = find_pci_device(VIRTIO_VENDOR, VIRTIO_9P_ID);
    if (!addr){ 
        kprintf("[VIRTIO_9P] device not found");
        return false;
    }

    pci_enable_device(addr);

    uint64_t disk_device_address, disk_device_size;

    virtio_get_capabilities(&np_dev, addr, &disk_device_address, &disk_device_size);
    pci_register(disk_device_address, disk_device_size);
    if (!virtio_init_device(&np_dev)) {
        kprintf("[VIRTIO_9P] Failed 9P initialization");
        return false;
    }

    kprintf("[VIRTIO_9P] Initialized device. Sending commands");

    max_msize = choose_version(); 

    open_files = chashmap_create(512);

    root = attach();
    if (root == INVALID_FID){
        kprintf("[VIRTIO_9P error] failed to attach");
        return false;
    }

    if (open(root) == INVALID_FID){
        kprintf("[VIRTIO_9P error] failed to open root directory");
        return false;
    }

    np_dev.common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    return true;
}

FS_RESULT Virtio9PDriver::open_file(const char* path, file* descriptor){
    uint64_t fid = reserve_fd_gid(path);
    descriptor->cursor = 0;
    descriptor->id = fid;

    irq_flags_t irq = irq_save_disable();
    module_file *cached = (module_file*)chashmap_get(open_files, &fid, sizeof(uint64_t));
    if (cached) {
        cached->references++;
        descriptor->size = cached->file_size;
        irq_restore(irq);
        return FS_RESULT_SUCCESS;
    }
    irq_restore(irq);
    uint32_t f = walk_dir(root, (char*)path);
    if (f == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to navigate to %s",path);
        return FS_RESULT_NOTFOUND;
    }
    r_getattr *attr = get_attribute(f, 0x00000200ULL);
    if (!attr) {
        clunk(&np_dev, f, mid++);
        return FS_RESULT_DRIVER_ERROR;
    }
    uint64_t size = read_unaligned64(&attr->size);
    p9_free(attr);
    descriptor->size = size;
    void* file = kalloc(np_dev.memory_page, size ? size : 1, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!file) {
        clunk(&np_dev, f, mid++);
        return FS_RESULT_DRIVER_ERROR;
    }
    if (open(f) == INVALID_FID){
        clunk(&np_dev, f, mid++);
        kfree(file, size ? size : 1);
        kprintf("[VIRTIO 9P error] failed to open %s",path);
        return FS_RESULT_DRIVER_ERROR;
    }
    if (size && read(f, 0, file) != size){
        clunk(&np_dev, f, mid++);
        kfree(file, size ? size : 1);
        kprintf("[VIRTIO 9P error] failed read file %s",path);
        return FS_RESULT_DRIVER_ERROR;
    } 
    clunk(&np_dev, f, mid++);
    irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &fid, sizeof(uint64_t));
    if (!mfile){
        mfile = (module_file*)kalloc(np_dev.memory_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
        if (!mfile) {
            irq_restore(irq);
            kfree(file, size ? size : 1);
            return FS_RESULT_DRIVER_ERROR;
        }
        memset(mfile, 0, sizeof(module_file));
        if (chashmap_put(open_files, &descriptor->id, sizeof(uint64_t), mfile) < 0) {
            irq_restore(irq);
            kfree(mfile, sizeof(module_file));
            kfree(file, size ? size : 1);
            return FS_RESULT_DRIVER_ERROR;
        }
        mfile->references = 1;
    } else {
        if (mfile->file_buffer.buffer) kfree(mfile->file_buffer.buffer, mfile->file_size ? mfile->file_size : 1);
        mfile->references++;
    }
    mfile->file_size = size;
    mfile->file_buffer = (buffer){
        .buffer = file,
        .buffer_size = size,
        .limit = size,
        .options = buffer_opt_none,
        .cursor = 0,
    };
    mfile->ignore_cursor = false;
    mfile->fid = descriptor->id;
    irq_restore(irq);
    return FS_RESULT_SUCCESS;
}

size_t Virtio9PDriver::read_file(file *descriptor, void* buf, size_t size){
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) {
        irq_restore(irq);
        return 0;
    }
    if (descriptor->cursor > mfile->file_size) {
        irq_restore(irq);
        return 0;
    }
    if (size > mfile->file_size-descriptor->cursor) size = mfile->file_size-descriptor->cursor;
    memcpy(buf, (char*)mfile->file_buffer.buffer + descriptor->cursor, size);
    irq_restore(irq);
    return size;
}

void Virtio9PDriver::close_file(file* descriptor){
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) {
        irq_restore(irq);
        return;
    }
    if (mfile->references) mfile->references--;
    if (mfile->references == 0){
        chashmap_remove(open_files, &descriptor->id, sizeof(uint64_t), 0);
        irq_restore(irq);
        if (mfile->file_buffer.buffer) kfree(mfile->file_buffer.buffer, mfile->file_size ? mfile->file_size : 1);
        kfree(mfile, sizeof(module_file));
        return;
    }
    irq_restore(irq);
}

size_t Virtio9PDriver::list_contents(const char *path, void* buf, size_t size, uint64_t *offset){
    uint32_t d = walk_dir(root, (char*)path);
    if (d == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to navigate to directory");
        return 0;
    }
    if (open(d) == INVALID_FID){
        clunk(&np_dev, d, mid++);
        kprintf("[VIRTIO 9P error] failed to open directory");
        return 0;
    }
    size_t amount = list_contents(d, buf, size, offset);
    clunk(&np_dev, d, mid++);
    return amount;
}

size_t Virtio9PDriver::choose_version(){
    p9_version_packet *cmd = make_p9_version_packet("9P2000.L", 0x1000000);
    p9_version_packet *resp = (p9_version_packet*)make_p9_response_buffer();
    
    virtio_buf b[2]={VBUF(cmd, sizeof(p9_version_packet), 0), VBUF(resp, sizeof(p9_version_packet), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2); 

    u32 msize = read_p9_version_max_size(resp);

    p9_free(cmd);
    p9_free(resp);

    return msize;
}

uint32_t Virtio9PDriver::attach(){
    t_attach *cmd = make_p9_attach_packet();
    void *resp = make_p9_response_buffer();
    
    virtio_buf b[2]= {VBUF(cmd, sizeof(t_attach), 0), VBUF(resp, sizeof(r_attach), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    uint32_t rid = check_9p_success(resp) ? read_unaligned32(&cmd->fid) : INVALID_FID;

    p9_free(cmd);
    p9_free(resp);

    return rid;
}

u32 Virtio9PDriver::open(u32 fid){
    t_lopen *cmd = make_p9_open_packet(fid);
    void *resp = make_p9_response_buffer();
    
    virtio_buf b[2] = {VBUF(cmd, sizeof(t_lopen), 0), VBUF(resp, sizeof(r_lopen), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    u32 rid = check_9p_success(resp) ? fid : INVALID_FID;
    
    p9_free(cmd);
    p9_free(resp);

    return rid;
}

typedef struct t_clunk {
    p9_packet_header header;
    uint32_t fid;
} __attribute__((packed)) t_clunk;

bool Virtio9PDriver::clunk(virtio_device *dev, uint32_t fid, uint16_t tag) {
    t_clunk *cmd = (t_clunk*)kalloc(dev->memory_page, sizeof(t_clunk), ALIGN_4KB, MEM_PRIV_KERNEL);
    t_clunk *resp = (t_clunk*)kalloc(dev->memory_page, sizeof(t_clunk), ALIGN_4KB, MEM_PRIV_KERNEL);
    if (!cmd || !resp) {
        if (cmd) kfree(cmd, sizeof(t_clunk));
        if (resp) kfree(resp, sizeof(t_clunk));
        return false;
    }

    write_unaligned32(&cmd->header.size, sizeof(t_clunk));
    cmd->header.id = P9_TCLUNK;
    write_unaligned16(&cmd->header.tag, tag);
    write_unaligned32(&cmd->fid, fid);

    virtio_buf b[2] = {VBUF(cmd, sizeof(t_clunk), 0), VBUF(resp, sizeof(t_clunk), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(dev, b, 2);

    bool ok = resp->header.id == P9_RCLUNK;
    kfree(cmd, sizeof(t_clunk));
    kfree(resp, sizeof(t_clunk));
    return ok;
}

size_t Virtio9PDriver::list_contents(u32 fid, void *buf, size_t size, u64 *offset){
    t_readdir *cmd = make_p9_readdir_packet(fid, (u32)size, offset ? *offset : 0);
    void* resp = make_p9_response_buffer();

    virtio_buf b[2]={VBUF(cmd, sizeof(t_readdir) ,0), VBUF((void*)resp, sizeof(r_readdir) + cmd->count, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    p9_free(cmd);
    
    if (!check_9p_success(resp)){
        p9_free(resp);
        kprintf("[VIRTIO 9P error] failed to get directory entries");
        return 0;
    }

    char *write_ptr = (char*)buf + 4;

    uintptr_t p = (uptr)resp + sizeof(r_readdir);
    uintptr_t end = p + read_unaligned32(&((r_readdir*)resp)->count);

    uint32_t count = 0;

    while (p + sizeof(r_readdir_data) <= end){
        r_readdir_data *data = (r_readdir_data*)p;
        uintptr_t name_ptr = p + sizeof(r_readdir_data);
        uintptr_t next = name_ptr + data->name_len;
        if (next > end) break;
    
        char *name = (char*)name_ptr;

        memcpy(write_ptr, name, data->name_len);
        write_ptr += data->name_len;
        *write_ptr++ = 0;
        count++;

    uint32_t size = read_unaligned32((void*)((uptr)resp + sizeof(p9_packet_header)));
        
        p = next;
    }

    *(uint32_t*)buf = count;

    p9_free((void*)resp);

    return (uintptr_t)write_ptr-(uintptr_t)buf;

}

uint32_t Virtio9PDriver::walk_dir(uint32_t fid, char *path){
    uint32_t amount = 0x1000;
    t_walk *cmd = make_p9_walk_packet(fid, path);
    void* resp = make_p9_response_buffer();

    virtio_buf b[2] = { VBUF(cmd, cmd->header.size, 0), VBUF((void*)resp, amount, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    uint32_t rid = check_9p_success(resp) ? read_unaligned32(&cmd->newfid) : INVALID_FID;
    p9_free(cmd);
    p9_free(resp);

    return rid;
}

r_getattr* Virtio9PDriver::get_attribute(u32 fid, u64 mask){
    t_getattr *cmd = make_p9_getattr_packet(fid, mask);
    r_getattr *resp = (r_getattr*)make_p9_response_buffer();

    virtio_buf b[2] = {VBUF(cmd, cmd->header.size, 0), VBUF(resp, sizeof(r_getattr), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    
    p9_free(cmd);
    if (!check_9p_success(resp)) {
        p9_free(resp);
        return 0;
    }

    return resp;
}

uint64_t Virtio9PDriver::read(u32 fid, u64 offset, void *file){
    uint32_t amount = 0x10000;
    t_read *cmd = make_p9_read_packet(fid, offset, amount);
    void* resp = make_p9_sized_buffer(amount);

    virtio_buf b[2] = {VBUF(cmd, sizeof(t_read), 0) ,VBUF(resp, amount, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    
    if (!check_9p_success(resp)) return 0;

    uint32_t size = *(uint32_t*)((uptr)resp + sizeof(p9_packet_header));
    
    memcpy((void*)((uptr)file + offset), (void*)((uptr)resp + sizeof(u32) + sizeof(p9_packet_header)), size);

    p9_free(cmd);
    p9_free(resp);

    if (size > 0) 
        return size + read(fid, offset + size, file);

    return size;

}

#define DIR_MASK 0x4000

bool Virtio9PDriver::stat(const char *path, fs_stat *out_stat){
    if (!path || !out_stat) return false;
    uint32_t f = walk_dir(root, (char*)path);
    if (f == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to navigate to %s",path);
        return false;
    }
    r_getattr *attr = get_attribute(f, 0x00000201ULL);
    if (!attr) {
        clunk(&np_dev, f, mid++);
        return false;
    }
    out_stat->size = read_unaligned64(&attr->size);
    out_stat->type = read_unaligned32(&attr->mode) & DIR_MASK ? entry_directory : entry_file;
    p9_free(attr);
    clunk(&np_dev, f, mid++);
    return true;
}
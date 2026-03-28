#include "virtio_9p_pci.hpp"
#include "virtio/virtio_pci.h"
#include "console/kio.h"
#include "pci.h"
#include "memory/page_allocator.h"
#include "exceptions/exception_handler.h"
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

    open_files = chashmap_create(128);

    root = attach();
    if (root == INVALID_FID){
        kprintf("[VIRTIO_9P error] failed to attach");
        return false;
    }

    if (open(root) == INVALID_FID){
        kprintf("[VIRTIO_9P error] failed to open root directory");
        return false;
    }

    return true;
}

FS_RESULT Virtio9PDriver::open_file(const char* path, file* descriptor){
    uint32_t f = walk_dir(root, (char*)path);
    if (f == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to navigate to %s",path);
        return FS_RESULT_NOTFOUND;
    }
    uint64_t fid = reserve_fd_gid(path);
    descriptor->cursor = 0;
    descriptor->id = fid;
    r_getattr *attr = get_attribute(f, 0x00000200ULL);
    if (!attr) return FS_RESULT_DRIVER_ERROR;
    uint64_t size = read_unaligned64(&attr->size);
    descriptor->size = size;
    void* file = kalloc(np_dev.memory_page, size, ALIGN_64B, MEM_PRIV_KERNEL);
    if (open(f) == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to open %s",path);
        return FS_RESULT_DRIVER_ERROR;
    }
    if (read(f, 0, file) != size){
        kprintf("[VIRTIO 9P error] failed read file %s",path);
        return FS_RESULT_DRIVER_ERROR;
    } 
    module_file *mfile = (module_file*)chashmap_get(open_files, &fid, sizeof(uint64_t));
    if (!mfile){
        mfile = (module_file*)kalloc(np_dev.memory_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
        if (chashmap_put(open_files, &descriptor->id, sizeof(uint64_t), mfile) < 0) return FS_RESULT_DRIVER_ERROR;
    } else {
        kfree((void*)mfile->buf, mfile->file_size);
    }
    mfile->file_size = size;
    mfile->buf = (uintptr_t)file;
    mfile->ignore_cursor = false;
    mfile->fid = descriptor->id;
    mfile->serial = f;
    mfile->references++;
    return FS_RESULT_SUCCESS;
}

size_t Virtio9PDriver::read_file(file *descriptor, void* buf, size_t size){
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return 0;
    if (descriptor->cursor > mfile->file_size) return 0;
    if (size > mfile->file_size-descriptor->cursor) size = mfile->file_size-descriptor->cursor;
    memcpy(buf, (void*)(mfile->buf + descriptor->cursor), size);
    return size;
}

size_t Virtio9PDriver::write_file(file *descriptor, const char* buf, size_t size){
    module_file *mfile  = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return 0;
    if (mfile->read_only) return 0;
    
    size_t written = buffer_write_to(&mfile->file_buffer, buf, size, descriptor->cursor);
    //TODO: sync should read the file from the server and sync it to the version in ram, the version in ram is not a source of truth
    
    write(mfile->serial, descriptor->cursor, size, buf);
    
    return written;
}

void Virtio9PDriver::close_file(file* descriptor){
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return;
    mfile->references--;
    if (mfile->references == 0){
        chashmap_remove(open_files, &descriptor->id, sizeof(uint64_t), 0);
        kfree((void*)mfile->buf, mfile->file_size);
    }
}

size_t Virtio9PDriver::list_contents(const char *path, void* buf, size_t size, uint64_t *offset){
    uint32_t d = walk_dir(root, (char*)path);
    if (d == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to navigate to directory");
        return 0;
    }
    if (open(d) == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to open directory");
    }
    kprintf("Directory opened and being read from %x",size);
    return list_contents(d, buf, size, offset);
}

size_t Virtio9PDriver::choose_version(){
    p9_version_packet *cmd = make_p9_version_packet("9P2000.L", 0x1000000);
    p9_version_packet *resp = (p9_version_packet*)make_p9_response_buffer();
    
    virtio_buf b[2] = {VBUF(cmd, sizeof(p9_version_packet), 0), VBUF(resp, sizeof(p9_version_packet), VIRTQ_DESC_F_WRITE)};
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
    
    virtio_buf b[2] = { VBUF(cmd, sizeof(t_lopen), 0), VBUF(resp, sizeof(r_lopen), VIRTQ_DESC_F_WRITE) };
    virtio_send_nd(&np_dev, b, 2);
    u32 rid = check_9p_success(resp) ? fid : INVALID_FID;
    
    p9_free(cmd);
    p9_free(resp);

    return rid;
}

size_t Virtio9PDriver::list_contents(u32 fid, void *buf, size_t size, u64 *offset){
    t_readdir *cmd = make_p9_readdir_packet(fid, (u32)size, offset ? *offset : 0);
    void* resp = make_p9_response_buffer();

    virtio_buf b[2]={VBUF(cmd, sizeof(t_readdir) ,0), VBUF(resp, sizeof(r_readdir) + cmd->count, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    p9_free(cmd);
    
    if (!check_9p_success(resp)){
        p9_free(resp);
        kprintf("[VIRTIO 9P error] failed to get directory entries");
        return 0;
    }

    char *write_ptr = (char*)buf + 4;

    uintptr_t p = (uptr)resp + sizeof(r_readdir);

    uint32_t count = 0;

    while (p < (uptr)resp + ((r_readdir*)resp)->count){
        r_readdir_data *data = (r_readdir_data*)p;
    
        char *name = (char*)(p + sizeof(r_readdir_data));

        count++;
        memcpy(write_ptr, name, data->name_len);
        write_ptr += data->name_len;
        *write_ptr++ = 0;

        if (offset) *offset = data->offset;
        
        p += sizeof(r_readdir_data) + data->name_len;
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
    r_getattr* resp = (r_getattr*)make_p9_response_buffer();

    virtio_buf b[2] = {VBUF(cmd, cmd->header.size, 0), VBUF(resp, sizeof(r_getattr), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    
    p9_free(cmd);
    p9_free(resp);

    return check_9p_success(resp) ? resp : 0;
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

uint64_t Virtio9PDriver::write(u32 fid, u64 offset, size_t amount, const char* buf){
    t_write *cmd = make_p9_write_packet(fid, offset, amount, buf);
    void* resp = make_p9_response_buffer();
    
    virtio_buf b[2] = {VBUF(cmd, cmd->header.size, 0), VBUF(resp, sizeof(r_write), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    
    p9_free(cmd);
    
    if (!check_9p_success(resp)){
        p9_free(resp);
        return 0;
    }
    
    r_write *response = (r_write*)resp;
    
    size_t written = response->count;
    
    p9_free(resp);
    
    return written;
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
    if (!attr) return false;
    out_stat->size = read_unaligned64(&attr->size);
    out_stat->type = read_unaligned32(&attr->mode) & DIR_MASK ? entry_directory : entry_file;
    return false;
}
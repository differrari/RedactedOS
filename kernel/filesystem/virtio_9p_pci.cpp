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
#include "filesystem/modules/module_loader.h"

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
    r_getattr *attr = get_attribute(f, P9_GETATTR_SIZE);
    if (!attr) {
        clunk(&np_dev, f);
        return FS_RESULT_DRIVER_ERROR;
    }
    uint64_t size = read_unaligned64(&attr->size);
    p9_free(attr);
    descriptor->size = size;
    void* file = kalloc(np_dev.memory_page, size ? size : 1, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!file) {
        clunk(&np_dev, f);
        return FS_RESULT_DRIVER_ERROR;
    }
    if (open(f) == INVALID_FID){
        clunk(&np_dev, f);
        kfree(file, size ? size : 1);
        kprintf("[VIRTIO 9P error] failed to open %s",path);
        return FS_RESULT_DRIVER_ERROR;
    }
    if (size && read(f, 0, file) != size){
        clunk(&np_dev, f);
        kfree(file, size ? size : 1);
        kprintf("[VIRTIO 9P error] failed read file %s",path);
        return FS_RESULT_DRIVER_ERROR;
    } 
    irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &fid, sizeof(uint64_t));
    if (!mfile){
        mfile = (module_file*)kalloc(np_dev.memory_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
        if (!mfile) {
            irq_restore(irq);
            clunk(&np_dev, f);
            kfree(file, size ? size : 1);
            return FS_RESULT_DRIVER_ERROR;
        }
        memset(mfile, 0, sizeof(module_file));
        mfile->serial = INVALID_FID;
        if (chashmap_put(open_files, &descriptor->id, sizeof(uint64_t), mfile) < 0) {
            irq_restore(irq);
            clunk(&np_dev, f);
            kfree(mfile, sizeof(module_file));
            kfree(file, size ? size : 1);
            return FS_RESULT_DRIVER_ERROR;
        }
    } else {
        if (mfile->serial != INVALID_FID) clunk(&np_dev, (uint32_t)mfile->serial);
        if (mfile->file_buffer.buffer) kfree(mfile->file_buffer.buffer, mfile->file_buffer.buffer_size ? mfile->file_buffer.buffer_size : 1);
    }
    mfile->file_size = size;
    mfile->buf = (uptr)file;
    mfile->file_buffer = (buffer){
        .buffer = file,
        .buffer_size = size,
        .limit = size,
        .options = buffer_opt_none,
        .cursor = 0,
    };
    mfile->ignore_cursor = false;
    mfile->fid = descriptor->id;
    mfile->serial = f;
    mfile->references++;
    irq_restore(irq);
    return FS_RESULT_SUCCESS;
}

size_t Virtio9PDriver::read_file(file *descriptor, void* buf, size_t size){
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    irq_restore(irq);
    if (!mfile) return 0;
    if (!sync_file(mfile) && !mfile->file_buffer.buffer && mfile->file_size) return 0;
    if (descriptor->cursor > mfile->file_size) return 0;
    if (size > mfile->file_size-descriptor->cursor) size = mfile->file_size-descriptor->cursor;
    memcpy(buf, (char*)mfile->file_buffer.buffer + descriptor->cursor, size);
    descriptor->cursor += size;
    descriptor->size = mfile->file_size;
    return size;
}

size_t Virtio9PDriver::write_file(file *descriptor, const char* buf, size_t size){
    module_file *mfile  = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return 0;
    if (mfile->read_only) return 0;

    size_t start = descriptor->cursor;
    size_t written = write((u32)mfile->serial, start, size, buf);
    if (!written) return 0;

    size_t end = start + written;
    if (end > mfile->file_buffer.buffer_size) {
        void *new_buf = kalloc(np_dev.memory_page, end ? end : 1, ALIGN_64B, MEM_PRIV_KERNEL);
        if (new_buf) {
            if (mfile->file_buffer.buffer && mfile->file_size) memcpy(new_buf, mfile->file_buffer.buffer, mfile->file_size);
            if (mfile->file_buffer.buffer) kfree(mfile->file_buffer.buffer, mfile->file_buffer.buffer_size ? mfile->file_buffer.buffer_size : 1);

            mfile->file_buffer.buffer = new_buf;
            mfile->file_buffer.buffer_size = end;
            mfile->buf = (uptr)new_buf;
        }
    }

    if (end > mfile->file_size) mfile->file_size = end;
    mfile->file_buffer.limit = mfile->file_size;

    if (mfile->file_buffer.buffer && start + written <= mfile->file_buffer.buffer_size) memcpy((char*)mfile->file_buffer.buffer + start, buf, written);

    descriptor->size = mfile->file_size;
    return written;
}

void Virtio9PDriver::close_file(file* descriptor){
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) {
        irq_restore(irq);
        return;
    }
    if (mfile->references > 1){
        mfile->references--;
        irq_restore(irq);
        return;
    }
    
    uint64_t serial = mfile->serial;
    void *buf = mfile->file_buffer.buffer;
    size_t buf_size = mfile->file_buffer.buffer_size ? mfile->file_buffer.buffer_size : 1;

    chashmap_remove(open_files, &descriptor->id, sizeof(uint64_t), 0);
    irq_restore(irq);

    if (serial != INVALID_FID) clunk(&np_dev, (u32)serial);
    if (buf) kfree(buf, buf_size);
    kfree(mfile, sizeof(module_file));
}

size_t Virtio9PDriver::list_contents(const char *path, void* buf, size_t size, uint64_t *offset){
    uint32_t d = walk_dir(root, (char*)path);
    if (d == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to navigate to directory");
        return 0;
    }
    if (open(d) == INVALID_FID){
        clunk(&np_dev, d);
        kprintf("[VIRTIO 9P error] failed to open directory");
        return 0;
    }
    size_t amount = list_contents(d, buf, size, offset);
    clunk(&np_dev, d);
    return amount;
}

bool Virtio9PDriver::truncate(file *descriptor, size_t size){
    module_file *mfile  = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return false;
    if (mfile->read_only) return false;
    if (!set_attribute((u32)mfile->serial, P9_SETATTR_SIZE, size)) return false;
    if (!sync_file(mfile)) return false;
    descriptor->size = mfile->file_size;
    if (descriptor->cursor > descriptor->size) descriptor->cursor = descriptor->size;
    return true;
}

size_t Virtio9PDriver::choose_version(){
    p9_version_packet *cmd = make_p9_version_packet("9P2000.L", 0x1000000);
    p9_version_packet *resp = (p9_version_packet*)make_p9_response_buffer();
    
    virtio_buf b[2]={VBUF(cmd, read_unaligned32(&cmd->header.size), 0), VBUF(resp, PAGE_SIZE, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2); 

    u32 msize = check_9p_success(resp) ? read_p9_version_max_size(resp) : 0;

    p9_free(cmd);
    p9_free(resp);

    return msize;
}

uint32_t Virtio9PDriver::attach(){
    t_attach *cmd = make_p9_attach_packet();
    void *resp = make_p9_response_buffer();
    
    virtio_buf b[2]= {VBUF(cmd, read_unaligned32(&cmd->header.size), 0), VBUF(resp, sizeof(r_attach), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    uint32_t rid = check_9p_success(resp) ? read_unaligned32(&cmd->fid) : INVALID_FID;

    p9_free(cmd);
    p9_free(resp);

    return rid;
}

u32 Virtio9PDriver::open(u32 fid){
    t_lopen *cmd = make_p9_open_packet(fid);
    void *resp = make_p9_response_buffer();
    
    virtio_buf b[2] = {VBUF(cmd, read_unaligned32(&cmd->header.size), 0), VBUF(resp, sizeof(r_lopen), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    u32 rid = check_9p_success(resp) ? fid : INVALID_FID;
    
    p9_free(cmd);
    p9_free(resp);

    return rid;
}

bool Virtio9PDriver::clunk(virtio_device *dev, uint32_t fid) {
    t_clunk *cmd = make_p9_clunk_packet(fid);
    void *resp = make_p9_response_buffer();
    if (!cmd || !resp) {
        if (cmd) p9_free(cmd);
        if (resp) p9_free(resp);
        return false;
    }

    virtio_buf b[2] = {VBUF(cmd, read_unaligned32(&cmd->header.size), 0), VBUF(resp, sizeof(p9_packet_header), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(dev, b, 2);

    bool ok = check_9p_success(resp) && ((p9_packet_header*)resp)->id == P9_RCLUNK;
    p9_free(cmd);
    p9_free(resp);
    return ok;
}

size_t Virtio9PDriver::list_contents(u32 fid, void *buf, size_t size, u64 *offset){
    if (!buf || size < sizeof(uint32_t)) return 0;

    uint32_t request_size = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
    if (max_msize > sizeof(r_readdir)) {
        uint32_t max_size = (uint32_t)(max_msize - sizeof(r_readdir));
        if (request_size > max_size) request_size = max_size;
    }
    if (!request_size) {
        *(uint32_t*)buf = 0;
        return sizeof(uint32_t);
    }

    t_readdir *cmd = make_p9_readdir_packet(fid, request_size, offset ? *offset : 0);
    void* resp = make_p9_sized_buffer(sizeof(r_readdir) + request_size);

    virtio_buf b[2]={VBUF(cmd, read_unaligned32(&cmd->header.size),0), VBUF((void*)resp, sizeof(r_readdir) + request_size, VIRTQ_DESC_F_WRITE)};
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
    if (end > (uintptr_t)resp + sizeof(r_readdir) + request_size) end = (uintptr_t)resp + sizeof(r_readdir) + request_size;

    uint32_t count = 0;
    u64 next_offset = offset ? *offset : 0;

    while (p + sizeof(r_readdir_data) <= end){
        r_readdir_data *data = (r_readdir_data*)p;
        uintptr_t name_ptr = p + sizeof(r_readdir_data);
        uint16_t name_len = read_unaligned16(&data->name_len);
        uintptr_t next = name_ptr + name_len;
        if (next > end) break;
        if ((uintptr_t)write_ptr + name_len + 1 > (uintptr_t)buf + size) break;
    
        char *name = (char*)name_ptr;

        memcpy(write_ptr, name, name_len);
        write_ptr += name_len;
        *write_ptr++ = 0;
        count++;
        next_offset = read_unaligned64(&data->offset);
        p = next;
    }

    *(uint32_t*)buf = count;
    if (offset && count) *offset = next_offset;

    p9_free((void*)resp);

    return (uintptr_t)write_ptr-(uintptr_t)buf;

}

uint32_t Virtio9PDriver::walk_dir(uint32_t fid, char *path){
    t_walk *cmd = make_p9_walk_packet(fid, path);

    uint16_t expected = read_unaligned16(&cmd->num_names);
    uint32_t amount = sizeof(p9_packet_header) + sizeof(uint16_t) + ((uint32_t)expected * 13);
    if (amount < sizeof(r_walk)) amount = sizeof(r_walk);
    void* resp = make_p9_sized_buffer(amount);

    virtio_buf b[2] = { VBUF(cmd, read_unaligned32(&cmd->header.size), 0), VBUF((void*)resp, amount, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    bool ok = check_9p_success(resp) && ((p9_packet_header*)resp)->id == P9_RWALK && read_unaligned16(&((r_walk*)resp)->num_qids) == expected;
    uint32_t rid = ok ? read_unaligned32(&cmd->newfid) : INVALID_FID;
    p9_free(cmd);
    p9_free(resp);

    return rid;
}

r_getattr* Virtio9PDriver::get_attribute(u32 fid, u64 mask){
    t_getattr *cmd = make_p9_getattr_packet(fid, mask);
    r_getattr *resp = (r_getattr*)make_p9_response_buffer();

    virtio_buf b[2] = {VBUF(cmd, read_unaligned32(&cmd->header.size), 0), VBUF(resp, sizeof(r_getattr), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    
    p9_free(cmd);
    if (!check_9p_success(resp)) {
        p9_free(resp);
        return 0;
    }

    return resp;
}

bool Virtio9PDriver::set_attribute(u32 fid, u64 mask, u64 value){
    t_setattr *cmd = make_p9_setattr_packet(fid, mask, value);
    void *resp = make_p9_response_buffer();

    virtio_buf b[2] = {VBUF(cmd, read_unaligned32(&cmd->header.size), 0),VBUF(resp, sizeof(p9_packet_header), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    bool ok = check_9p_success(resp);

    p9_free(cmd);
    p9_free(resp);

    return ok;
}

uint64_t Virtio9PDriver::read(u32 fid, u64 offset, void *file){
    uint32_t amount = 0x10000;
    if (max_msize > sizeof(p9_packet_header) + sizeof(u32)) {
        uint32_t transport_limit = (uint32_t)(max_msize - sizeof(p9_packet_header) - sizeof(u32));
        if (transport_limit < amount) amount = transport_limit;
    }

    uint64_t total = 0;
    while (1) {
        t_read *cmd = make_p9_read_packet(fid, offset + total, amount);
        void* resp = make_p9_sized_buffer(sizeof(p9_packet_header) + sizeof(u32) + amount);
        
        virtio_buf b[2] = {VBUF(cmd, read_unaligned32(&cmd->header.size), 0) ,VBUF(resp, sizeof(p9_packet_header) + sizeof(u32) + amount, VIRTQ_DESC_F_WRITE)};
        virtio_send_nd(&np_dev, b, 2);
    
        if (!check_9p_success(resp)) {
            p9_free(cmd);
            p9_free(resp);
            break;
        }

        uint32_t got = read_unaligned32((void*)((uptr)resp + sizeof(p9_packet_header)));
        if (got > amount) {
            p9_free(cmd);
            p9_free(resp);
            break;
        }

        memcpy((void*)((uptr)file + total), (void*)((uptr)resp + sizeof(p9_packet_header) + sizeof(u32)), got);

        p9_free(cmd);
        p9_free(resp);

        total += got;
        if (!got) break;
    }

    return total;
}

size_t Virtio9PDriver::write(u32 fid, u64 offset, size_t amount, const char* buf){
    if (!amount) return 0;

    size_t total_written = 0;
    size_t max_payload = 0;
    if (max_msize > sizeof(t_write)) max_payload = max_msize - sizeof(t_write);
    if (!max_payload) max_payload = amount;

    while (total_written < amount) {
        size_t chunk = amount - total_written;
        if (chunk > max_payload) chunk = max_payload;

        t_write *cmd = make_p9_write_packet(fid, offset + total_written, chunk, buf + total_written);
        void* resp = make_p9_response_buffer();
        virtio_buf b[2] = {VBUF(cmd, read_unaligned32(&cmd->header.size), 0), VBUF(resp, sizeof(r_write), VIRTQ_DESC_F_WRITE)};
        virtio_send_nd(&np_dev, b, 2);

        p9_free(cmd);

        if (!check_9p_success(resp)){
            p9_free(resp);
            break;
        }

        size_t written = read_unaligned32(&((r_write*)resp)->count);
        if (written > chunk) written = chunk;
        p9_free(resp);

        total_written += written;
        if (written != chunk) break;
    }

    return total_written;
}

#define DIR_MASK 0x4000

bool Virtio9PDriver::stat(const char *path, fs_stat *out_stat){
    if (!path || !out_stat) return false;
    uint32_t f = walk_dir(root, (char*)path);
    if (f == INVALID_FID){
        kprintf("[VIRTIO 9P error] failed to navigate to %s",path);
        return false;
    }
    r_getattr *attr = get_attribute(f, P9_GETATTR_SIZE | P9_GETATTR_MODE);
    if (!attr) {
        clunk(&np_dev, f);
        return false;
    }
    out_stat->size = read_unaligned64(&attr->size);
    out_stat->type = read_unaligned32(&attr->mode) & DIR_MASK ? entry_directory : entry_file;
    p9_free(attr);
    clunk(&np_dev, f);
    return true;
}

bool Virtio9PDriver::sync_file(module_file *mfile){
    if (!mfile || mfile->serial == INVALID_FID) return false;

    r_getattr *attr = get_attribute((u32)mfile->serial, P9_GETATTR_SIZE | P9_GETATTR_MODE);
    if (!attr) return false;

    size_t new_size = read_unaligned64(&attr->size);
    p9_free(attr);

    if (!new_size) {
        if (mfile->file_buffer.buffer) kfree(mfile->file_buffer.buffer, mfile->file_buffer.buffer_size ? mfile->file_buffer.buffer_size : 1);
        mfile->file_buffer.buffer = 0;
        mfile->file_buffer.buffer_size = 0;
        mfile->file_buffer.limit = 0;
        mfile->buf = 0;
        mfile->file_size = 0;
        return true;
    }

    bool replace_buffer = new_size != mfile->file_buffer.buffer_size;
    void *new_buf = mfile->file_buffer.buffer;
    if (replace_buffer) {
        new_buf = kalloc(np_dev.memory_page, new_size, ALIGN_64B, MEM_PRIV_KERNEL);
        if (!new_buf) return false;
    }

    if (read((u32)mfile->serial, 0, new_buf) != new_size) {
        if (replace_buffer) kfree(new_buf, new_size);
        kprintf("[VIRTIO 9P error] failed to sync file with serial %x", (u32)mfile->serial);
        return false;
    }

    if (replace_buffer) {
        if (mfile->file_buffer.buffer) kfree(mfile->file_buffer.buffer, mfile->file_buffer.buffer_size ? mfile->file_buffer.buffer_size : 1);
        mfile->file_buffer.buffer = new_buf;
        mfile->file_buffer.buffer_size = new_size;
    }

    mfile->file_buffer.limit = new_size;
    mfile->buf = (uptr)mfile->file_buffer.buffer;
    mfile->file_size = new_size;
    return true;
}

Virtio9PDriver *p9Driver;

bool shared_init(){
    if (BOARD_TYPE != 1) return false;
    p9Driver = new Virtio9PDriver();
    bool success = p9Driver->init(0);
    return success;
}

bool shared_fini(){
    return false;
}

FS_RESULT shared_open(const char *path, file *out_fd){
    return p9Driver->open_file(path, out_fd);
}

size_t shared_read(file *fd, char *out_buf, size_t size, file_offset offset){
    return p9Driver->read_file(fd, out_buf, size);
}

size_t shared_write(file *fd, const char *buf, size_t size, file_offset offset){
    return p9Driver->write_file(fd, buf, size);
}

size_t shared_readdir(const char* path, void *out_buf, size_t size, file_offset *offset){
    return p9Driver->list_contents(path, out_buf, size, offset);
}

bool shared_stat(const char *path, fs_stat *out_stat){
    return p9Driver->stat(path, out_stat);
}

void shared_close(file *descriptor){
    kprintf("9P will close file");
    p9Driver->close_file(descriptor);
}

bool shared_truncate(file *descriptor, size_t size){
    return p9Driver->truncate(descriptor, size);
}

system_module p9_fs_module = (system_module){
    .name = "9PFS",
    .mount = "home",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = shared_init,
    .fini = shared_fini,
    .open = shared_open,
    .read = shared_read,
    .write = shared_write,
    .close = shared_close,
    .truncate = shared_truncate,
    .getstat = shared_stat,
    .readdir = shared_readdir,
};

bool alias_p9_init(){
    return true;
}

system_module p9_fs_module_alias = (system_module){
    .name = "9PFS",
    .mount = "shared",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = alias_p9_init,
    .fini = shared_fini,
    .open = shared_open,
    .read = shared_read,
    .write = shared_write,
    .close = shared_close,
    .truncate = shared_truncate,
    .getstat = shared_stat,
    .readdir = shared_readdir,
};

extern "C" bool load_home(){
    return load_module(&p9_fs_module) && load_module(&p9_fs_module_alias);
}
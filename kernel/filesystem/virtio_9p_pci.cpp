#include "virtio_9p_pci.hpp"
#include "virtio/virtio_pci.h"
#include "console/kio.h"
#include "pci.h"
#include "memory/page_allocator.h"
#include "exceptions/exception_handler.h"
#include "std/memory.h"
#include "std/memory_access.h"

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
    uint64_t size = get_attribute(f, 0x00000200ULL);
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
        kfree((void*)mfile->buffer, mfile->file_size);
    }
    mfile->file_size = size;
    mfile->buffer = (uintptr_t)file;
    mfile->ignore_cursor = false;
    mfile->fid = descriptor->id;
    mfile->references++;
    return FS_RESULT_SUCCESS;
}

size_t Virtio9PDriver::read_file(file *descriptor, void* buf, size_t size){
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return 0;
    if (descriptor->cursor > mfile->file_size) return 0;
    if (size > mfile->file_size-descriptor->cursor) size = mfile->file_size-descriptor->cursor;
    memcpy(buf, (void*)(mfile->buffer + descriptor->cursor), size);
    return size;
}

void Virtio9PDriver::close_file(file* descriptor){
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return;
    mfile->references--;
    if (mfile->references == 0){
        chashmap_remove(open_files, &descriptor->id, sizeof(uint64_t), 0);
        kfree((void*)mfile->buffer, mfile->file_size);
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

enum {
    P9_TLERROR = 6,
    P9_RLERROR,
    P9_TSTATFS = 8,
    P9_RSTATFS,
    P9_TLOPEN = 12,
    P9_RLOPEN,
    P9_TLCREATE = 14,
    P9_RLCREATE,
    P9_TSYMLINK = 16,
    P9_RSYMLINK,
    P9_TMKNOD = 18,
    P9_RMKNOD,
    P9_TRENAME = 20,
    P9_RRENAME,
    P9_TREADLINK = 22,
    P9_RREADLINK,
    P9_TGETATTR = 24,
    P9_RGETATTR,
    P9_TSETATTR = 26,
    P9_RSETATTR,
    P9_TXATTRWALK = 30,
    P9_RXATTRWALK,
    P9_TXATTRCREATE = 32,
    P9_RXATTRCREATE,
    P9_TREADDIR = 40,
    P9_RREADDIR,
    P9_TFSYNC = 50,
    P9_RFSYNC,
    P9_TLOCK = 52,
    P9_RLOCK,
    P9_TGETLOCK = 54,
    P9_RGETLOCK,
    P9_TLINK = 70,
    P9_RLINK,
    P9_TMKDIR = 72,
    P9_RMKDIR,
    P9_TRENAMEAT = 74,
    P9_RRENAMEAT,
    P9_TUNLINKAT = 76,
    P9_RUNLINKAT,
    P9_TVERSION = 100,
    P9_RVERSION,
    P9_TAUTH = 102,
    P9_RAUTH,
    P9_TATTACH = 104,
    P9_RATTACH,
    P9_TERROR = 106,
    P9_RERROR,
    P9_TFLUSH = 108,
    P9_RFLUSH,
    P9_TWALK = 110,
    P9_RWALK,
    P9_TOPEN = 112,
    P9_ROPEN,
    P9_TCREATE = 114,
    P9_RCREATE,
    P9_TREAD = 116,
    P9_RREAD,
    P9_TWRITE = 118,
    P9_RWRITE,
    P9_TCLUNK = 120,
    P9_RCLUNK,
    P9_TREMOVE = 122,
    P9_RREMOVE,
    P9_TSTAT = 124,
    P9_RSTAT,
    P9_TWSTAT = 126,
    P9_RWSTAT,
};

typedef struct p9_version_packet {
    p9_packet_header header;
    uint32_t msize;
    uint16_t str_size;
    char buffer[8];
}__attribute__((packed)) p9_version_packet;
static_assert(sizeof(p9_version_packet) == 21, "Wrong version packet size");

void Virtio9PDriver::p9_max_tag(p9_packet_header* header){
    write_unaligned16(&header->tag,UINT16_MAX);
}

void Virtio9PDriver::p9_inc_tag(p9_packet_header* header){
    write_unaligned16(&header->tag,mid++);
}

size_t Virtio9PDriver::choose_version(){
    p9_version_packet *cmd = (p9_version_packet*)kalloc(np_dev.memory_page, sizeof(p9_packet_header), ALIGN_4KB, MEM_PRIV_KERNEL);
    p9_version_packet *resp = (p9_version_packet*)kalloc(np_dev.memory_page, sizeof(p9_packet_header), ALIGN_4KB, MEM_PRIV_KERNEL);
    
    cmd->header.size = sizeof(p9_version_packet);
    cmd->header.id = P9_TVERSION;
    p9_max_tag(&cmd->header);
    
    cmd->msize = 0x1000000;
    cmd->str_size = 8;
    memcpy(cmd->buffer,"9P2000.L",8);
    
    virtio_buf b[2]={VBUF(cmd, sizeof(p9_version_packet), 0), VBUF(resp, sizeof(p9_version_packet), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2); 

    uint64_t msize = resp->msize;

    kfree(cmd, sizeof(p9_packet_header));
    kfree(resp, sizeof(p9_packet_header));

    return msize;
}

typedef struct t_attach {
    p9_packet_header header;
    uint32_t fid;
    uint32_t afid;
    uint16_t uname_len;
    char uname[8];
    uint16_t aname_len;
    char aname[1];
    uint32_t n_uname;
}__attribute__((packed)) t_attach;

typedef struct r_attach {
    p9_packet_header header;
    uint8_t qid[13];
}__attribute__((packed)) r_attach;

uint32_t Virtio9PDriver::attach(){
    t_attach *cmd = (t_attach*)kalloc(np_dev.memory_page, sizeof(t_attach), ALIGN_4KB, MEM_PRIV_KERNEL);
    r_attach *resp = (r_attach*)kalloc(np_dev.memory_page, sizeof(r_attach), ALIGN_4KB, MEM_PRIV_KERNEL);

    cmd->header.size = sizeof(t_attach);
    cmd->header.id = P9_TATTACH;
    p9_inc_tag(&cmd->header);
    
    uint32_t fid = vfid++;
    cmd->fid = fid;
    cmd->uname_len = 8; 
    cmd->n_uname = 12345;//TODO: hash (name+timestamp) or random
    memcpy(cmd->uname,"REDACTED",8);
    
    virtio_buf b[2]= {VBUF(cmd, sizeof(t_attach), 0), VBUF(resp, sizeof(r_attach), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    uint32_t rid = resp->header.id == P9_RLERROR ? INVALID_FID : fid;

    kfree(cmd, sizeof(t_attach));
    kfree(resp, sizeof(r_attach));

    return rid;
}

typedef struct t_lopen {
    p9_packet_header header;
    uint32_t fid; 
    uint32_t flags;
}__attribute__((packed)) t_lopen;

typedef struct r_lopen {
    p9_packet_header header;
    uint8_t qid[13]; 
    uint32_t iounit;
}__attribute__((packed)) r_lopen;

#define O_RDONLY         00
#define O_WRONLY         01
#define O_RDWR           02

uint32_t Virtio9PDriver::open(uint32_t fid){
    t_lopen *cmd = (t_lopen*)kalloc(np_dev.memory_page, sizeof(t_lopen), ALIGN_4KB, MEM_PRIV_KERNEL);
    r_lopen *resp = (r_lopen*)kalloc(np_dev.memory_page, sizeof(r_lopen), ALIGN_4KB, MEM_PRIV_KERNEL);
    
    cmd->header.size = sizeof(t_lopen);
    cmd->header.id = P9_TLOPEN;
    p9_inc_tag(&cmd->header);

    cmd->fid = fid;
    cmd->flags = O_RDONLY;
    
    virtio_buf b[2] = {VBUF(cmd, sizeof(t_lopen), 0), VBUF(resp, sizeof(r_lopen), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    uint32_t rid = resp->header.id == P9_RLERROR ? INVALID_FID : fid;
    
    kfree(cmd, sizeof(t_lopen));
    kfree(resp, sizeof(r_lopen));

    return rid;
}

typedef struct t_readdir {
    p9_packet_header header;
    uint32_t fid;
    uint64_t offset;
    uint32_t count;
}__attribute__((packed)) t_readdir;

typedef struct r_readdir_data {
    uint8_t qid[13];
    uint64_t offset;
    uint8_t type;
    uint16_t name_len;
    // Followed by name;
}__attribute__((packed)) r_readdir_data;

typedef struct r_readdir {
    p9_packet_header header;
    uint32_t count;
    // Followed by data
}__attribute__((packed)) r_readdir;

size_t Virtio9PDriver::list_contents(uint32_t fid, void *buf, size_t size, uint64_t *offset){
    t_readdir *cmd = (t_readdir*)kalloc(np_dev.memory_page, sizeof(t_readdir), ALIGN_4KB, MEM_PRIV_KERNEL);
    uintptr_t resp = (uintptr_t)kalloc(np_dev.memory_page, sizeof(r_readdir) + size, ALIGN_4KB, MEM_PRIV_KERNEL);
    
    cmd->header.size = sizeof(t_readdir);
    cmd->header.id = P9_TREADDIR;
    p9_inc_tag(&cmd->header);
    
    cmd->fid = fid;
    cmd->count = size;
    cmd->offset = offset ? *offset : 0;

    virtio_buf b[2]={VBUF(cmd, sizeof(t_readdir) ,0), VBUF((void*)resp, sizeof(r_readdir) + cmd->count, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    kfree(cmd, sizeof(t_readdir));
    
    if (((r_readdir*)resp)->header.id == P9_RLERROR){
        kfree((void*)resp, sizeof(r_readdir) + size);
        kprintf("[VIRTIO 9P error] failed to get directory entries");
        return 0;
    }

    char *write_ptr = (char*)buf + 4;

    uintptr_t p = resp + sizeof(r_readdir);

    uint32_t count = 0;

    while (p < resp + ((r_readdir*)resp)->count){
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


    kfree((void*)resp, sizeof(r_readdir) + size);

    return (uintptr_t)write_ptr-(uintptr_t)buf;

}

typedef struct t_walk {
    p9_packet_header header;
    uint32_t fid;
    uint32_t newfid;
    uint16_t num_names;
}__attribute__((packed)) t_walk;

uint32_t Virtio9PDriver::walk_dir(uint32_t fid, char *path){
    uint32_t amount = 0x1000;
    t_walk *cmd = (t_walk*)kalloc(np_dev.memory_page, sizeof(t_walk) + amount, ALIGN_4KB, MEM_PRIV_KERNEL);
    uintptr_t resp = (uintptr_t)kalloc(np_dev.memory_page, amount, ALIGN_4KB, MEM_PRIV_KERNEL);
    
    cmd->header.id = P9_TWALK;
    p9_inc_tag(&cmd->header);

    uint32_t nfid = vfid++;
    
    cmd->fid = fid;
    cmd->newfid = nfid;

    uintptr_t p = (uintptr_t)cmd + sizeof(t_walk);

    char *new_path = path;
    while (*new_path != 0) {
        new_path = (char*)seek_to(new_path, '/');
        uint8_t offset = new_path-path-(*new_path != 0 || *(new_path-1) == '/');
        if (offset != 0){
            cmd->num_names++;
            *(uint16_t*)p = offset;
            p += 2;
            memcpy((void*)p, path, offset);
            p += offset;
        }
        path = new_path;
    }
    
    cmd->header.size = p-(uintptr_t)cmd;

    virtio_buf b[2] = { VBUF(cmd, cmd->header.size, 0), VBUF((void*)resp, amount, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);

    uint32_t rid =  ((p9_packet_header*)resp)->id == P9_RLERROR ? INVALID_FID : nfid;
    kfree((void*)cmd, sizeof(t_walk) + amount);
    kfree((void*)resp, amount);

    return rid;
}

typedef struct t_getattr {
    p9_packet_header header;
    uint32_t fid;
    uint64_t mask;    
}__attribute__((packed)) t_getattr;

typedef struct r_getattr {
    p9_packet_header header;
    uint64_t valid;
    uint8_t qid[13];
    uint32_t model;
    uint32_t uid;
    uint32_t gid;
    uint64_t nlink;
    uint64_t rdev;
    uint64_t size;
    uint64_t blksize;
    uint64_t blocks;
    uint64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, btime_sec, btime_nsec;
    uint64_t gen;
    uint64_t data_version;
}__attribute__((packed)) r_getattr;

uint64_t Virtio9PDriver::get_attribute(uint32_t fid, uint64_t mask){
    t_getattr *cmd = (t_getattr*)kalloc(np_dev.memory_page, sizeof(t_getattr), ALIGN_4KB, MEM_PRIV_KERNEL);
    r_getattr *resp = (r_getattr*)kalloc(np_dev.memory_page, sizeof(r_getattr), ALIGN_4KB, MEM_PRIV_KERNEL);
    
    cmd->header.id = P9_TGETATTR;
    p9_inc_tag(&cmd->header);
    cmd->header.size = sizeof(t_getattr);
    cmd->fid = fid;
    cmd->mask = mask;

    virtio_buf b[2] = {VBUF(cmd, cmd->header.size, 0), VBUF(resp, sizeof(r_getattr), VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    uint64_t attr = resp->header.id == P9_RLERROR ? 0 : resp->size;
    
    kfree((void*)cmd, sizeof(t_getattr));
    kfree((void*)resp, sizeof(r_getattr));

    return attr;
}

typedef struct t_read {
    p9_packet_header header;
    uint32_t fid;
    uint64_t offset;
    uint32_t count;
}__attribute__((packed)) t_read;
static_assert(sizeof(t_read) == sizeof(t_readdir), "Wrong size");

uint64_t Virtio9PDriver::read(uint32_t fid, uint64_t offset, void *file){
    uint32_t amount = 0x10000;
    t_read *cmd = (t_read*)kalloc(np_dev.memory_page, sizeof(t_read), ALIGN_4KB, MEM_PRIV_KERNEL);
    uintptr_t resp = (uintptr_t)kalloc(np_dev.memory_page, amount, ALIGN_4KB, MEM_PRIV_KERNEL);
    
    cmd->header.size = sizeof(t_read);
    cmd->header.id = P9_TREAD;
    p9_inc_tag(&cmd->header);
    
    cmd->fid = fid;
    cmd->offset = offset;
    cmd->count = amount - sizeof(p9_packet_header) - sizeof(uint32_t);

    virtio_buf b[2] = {VBUF(cmd, sizeof(t_read), 0) ,VBUF((void*)resp, amount, VIRTQ_DESC_F_WRITE)};
    virtio_send_nd(&np_dev, b, 2);
    
    if (((p9_packet_header*)resp)->id == P9_RLERROR) return 0;

    uint32_t size = *(uint32_t*)(resp + sizeof(p9_packet_header));
    
    memcpy((void*)((uintptr_t)file + offset), (void*)(resp + sizeof(uint32_t) + sizeof(p9_packet_header)), size);

    kfree((void*)cmd, sizeof(t_read));
    kfree((void*)resp, amount);

    if (size > 0) 
        return size + read(fid, offset + size, file);

    return size;

}

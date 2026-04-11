#include "filesystem.h"
#include "fat32.hpp"
#include "mbr.h"
#include "fsdriver.hpp"
#include "std/std.h"
#include "console/kio.h"
#include "dev/module_loader.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "math/math.h"
#include "virtio_9p_pci.hpp"
#include "hw/hw.h"
#include "process/scheduler.h"
#include "pipe.h"
#include "theme/theme.h"
#include "files/dir_list.h"

FAT32FS *fs_driver;

typedef struct {
    uint64_t mfile_id;
    uint64_t file_id;
    size_t file_size;
    uint16_t pid;
    system_module* mod;
} open_file_descriptors;

chashmap_t *open_files;

bool boot_partition_init(){
    uint32_t f32_partition = mbr_find_partition(0xC);
    fs_driver = new FAT32FS();
    return fs_driver->init(f32_partition);
}

bool boot_partition_fini(){
    return false;
}

FS_RESULT boot_partition_open(const char *path, file *out_fd){
    return fs_driver->open_file(path, out_fd);
}

size_t boot_partition_read(file *fd, char *out_buf, size_t size, file_offset offset){
    return fs_driver->read_file(fd, out_buf, size);
}

size_t boot_partition_write(file *fd, const char *buf, size_t size, file_offset offset){
    return fs_driver->write_file(fd, buf, size);
}

size_t boot_partition_readdir(const char* path, void *out_buf, size_t size, file_offset *offset){
    return fs_driver->list_contents(path, out_buf, size, offset);
}

void boot_partition_close(file *descriptor){
    fs_driver->close_file(descriptor);
}

bool boot_stat(const char *path, fs_stat *out_stat){
    return fs_driver->stat(path, out_stat);
}

bool boot_truncate(file *descriptor, size_t size){
    return fs_driver->truncate(descriptor, size);
}

system_module boot_fs_module = (system_module){
    .name = "boot",
    .mount = "boot",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = boot_partition_init,
    .fini = boot_partition_fini,
    .open = boot_partition_open,
    .read = boot_partition_read,
    .write = boot_partition_write,
    .close = boot_partition_close,
    .truncate = boot_truncate,
    .sread = 0,
    .swrite = 0,
    .getstat = boot_stat,
    .readdir = boot_partition_readdir,
};

Virtio9PDriver *p9Driver;

bool shared_init(){
    if (BOARD_TYPE != 1) return false;
    p9Driver = new Virtio9PDriver();
    bool success = p9Driver->init(0);
    if (!success)
        system_config.app_directory = (char*)"boot";
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

size_t shared_sread(const char *path, void *buf, size_t size){
    return p9Driver->sread_file(path, buf, size);
}

size_t shared_swrite(const char *path, const void *buf, size_t size){
    return p9Driver->swrite_file(path, buf, size);
}

bool shared_truncate(file *descriptor, size_t size){
    return p9Driver->truncate(descriptor, size);
}

system_module p9_fs_module = (system_module){
    .name = "9PFS",
    .mount = "shared",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = shared_init,
    .fini = shared_fini,
    .open = shared_open,
    .read = shared_read,
    .write = shared_write,
    .close = shared_close,
    .truncate = shared_truncate,
    .sread = shared_sread,
    .swrite = shared_swrite,
    .getstat = shared_stat,
    .readdir = shared_readdir,
};

void* page;

void* open_files_alloc(size_t size){
    return kalloc(page, size, ALIGN_64B, MEM_PRIV_KERNEL);
}

bool init_filesystem(){
    page = palloc(PAGE_SIZE*8, MEM_PRIV_KERNEL, MEM_RW, false);
    open_files = chashmap_create(1024);
    open_files->alloc = open_files_alloc;
    open_files->free = kfree;
    const char *path = "disk";
    system_module *disk_mod = get_module(&path);
    if (!disk_mod) return false;
    return load_module(&boot_fs_module) && load_module(&p9_fs_module);
}

FS_RESULT open_file_global(const char* path, file* descriptor, system_module **mod){
    const char *search_path = path;
    if (*search_path == '/') search_path++;
    if (!*search_path) return FS_RESULT_NOTFOUND;
    system_module *module = get_module(&search_path);
    if (!module) return FS_RESULT_NOTFOUND;
    if (!module->open) return FS_RESULT_NOTFOUND;
    FS_RESULT result = module->open(search_path, descriptor);
    if (result != FS_RESULT_SUCCESS) return result;
    if (!open_files) return FS_RESULT_DRIVER_ERROR;
    descriptor->cursor = 0;
    *mod = module;
    return FS_RESULT_SUCCESS;
}

FS_RESULT open_file(const char* path, file* descriptor){
    system_module *mod = 0;
    FS_RESULT result = open_file_global(path, descriptor, &mod);
    if (result != FS_RESULT_SUCCESS) return result;
    open_file_descriptors *of = (open_file_descriptors*)kalloc(page, sizeof(open_file_descriptors), ALIGN_16B, MEM_PRIV_KERNEL);
    if (!of) {
        close_file_global(descriptor, mod);
        return FS_RESULT_DRIVER_ERROR;
    }
    of->mfile_id = descriptor->id;
    of->file_id = reserve_fd_id();
    of->file_size = descriptor->size;
    of->mod = mod;
    of->pid = get_current_proc_pid();
    descriptor->id = of->file_id;
    irq_flags_t irq = irq_save_disable();
    int put = chashmap_put(open_files, &of->file_id, sizeof(uint64_t), of);
    irq_restore(irq);

    if (put != 1) {
        file tmp = {
            .id = of->mfile_id,
            .size = of->file_size,
            .cursor = 0,
        };
        close_file_global(&tmp, mod);
        kfree(of, sizeof(open_file_descriptors));
        return FS_RESULT_DRIVER_ERROR;
    }
    return FS_RESULT_SUCCESS;
}

size_t read_file(file *descriptor, char* buf, size_t size){
    if (!open_files){
        kprintf("[FS] No open files");
        return 0;
    }
    open_file_descriptors local = {};
    irq_flags_t irq = irq_save_disable();
    open_file_descriptors *ofile = (open_file_descriptors *)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (ofile) local = *ofile;
    irq_restore(irq);
    if (!ofile || !local.mod || !local.mod->read || local.pid != get_current_proc_pid()) return 0;
    size_t start_cursor = descriptor->cursor;
    file gfd = (file){
        .id = local.mfile_id,
        .size = descriptor->size,
        .cursor = start_cursor,
    };
    size_t amount_read = local.mod->read(&gfd, buf, size, start_cursor);
    descriptor->cursor = gfd.cursor != start_cursor ? gfd.cursor : start_cursor + amount_read;
    descriptor->size = gfd.size;
    return amount_read;
}

void close_file(file *descriptor){
    if (!open_files || !descriptor) return;
    open_file_descriptors *ofile = 0;
    irq_flags_t irq = irq_save_disable();
    chashmap_remove(open_files, &descriptor->id, sizeof(uint64_t), (void**)&ofile);
    irq_restore(irq);
    if (!ofile) return;
    file gfd = (file){
        .id = ofile->mfile_id,
        .size = ofile->file_size,
        .cursor = descriptor->cursor,
    };
    close_file_global(&gfd, ofile->mod);
    kfree(ofile, sizeof(open_file_descriptors));
}

void close_file_global(file *descriptor, system_module *mod){
    if (!mod || !mod->close) return;
    mod->close(descriptor);
}

size_t write_file(file *descriptor, const char* buf, size_t size){
    if (descriptor->id == FD_OUT){
        const char *search_path = "proc";//TODO: This is ugly
        system_module *mod = get_module(&search_path);
        if (!mod || !mod->write) return 0;
        return mod->write(descriptor, buf, size, descriptor->cursor);
        //TODO: this handles its own cursor movement, but with the new sync policies it shouldn't have to
    }
    if (!open_files) return 0;

    open_file_descriptors local = {};
    irq_flags_t irq = irq_save_disable();
    open_file_descriptors *ofile = (open_file_descriptors *)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (ofile) local = *ofile;
    irq_restore(irq);
    if (!ofile || !local.mod || !local.mod->write || local.pid != get_current_proc_pid()) return 0;
    size_t start_cursor = descriptor->cursor;
    file gfd = (file){
        .id = local.mfile_id,
        .size = descriptor->size,
        .cursor = start_cursor,
    };
    size_t amount_written = local.mod->write(&gfd, buf, size, 0);
    descriptor->cursor = gfd.cursor != start_cursor ? gfd.cursor : start_cursor + amount_written;
    descriptor->size = gfd.size;
    irq = irq_save_disable();
    ofile = (open_file_descriptors *)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (ofile) ofile->file_size = gfd.size;
    irq_restore(irq);

    update_pipes(local.mfile_id, buf, amount_written);
    return amount_written;
}

size_t simple_read(const char *path, void *buf, size_t size){
    const char *search_path = path;
    if (*search_path == '/') search_path++;
    if (!*search_path) return 0;
    system_module *mod = get_module(&search_path);
    if (!mod) return 0;
    if (!mod->sread) return 0;
    return mod->sread(search_path, buf, size);
}

size_t simple_write(const char *path, const void *buf, size_t size){
    const char *search_path = path;
    if (*search_path == '/') search_path++;
    if (!*search_path) return 0;
    system_module *mod = get_module(&search_path);
    if (!mod) return 0;
    if (!mod->swrite) return 0;
    uint64_t mfid = reserve_fd_gid(path);
    size_t amount_written = mod->swrite(search_path, buf, size);
    update_pipes(mfid, (char*)buf, amount_written);
    return amount_written;
}

size_t list_directory_contents(const char *path, void* buf, size_t size, uint64_t *offset){
    const char *search_path = path;
    if (!search_path) return 0;
    if (*search_path == '/') search_path++;
    if (!*search_path){
        return list_root(buf, size, offset);
    }
    system_module *mod = get_module(&search_path);
    if (!mod){
        kprintf("No module for path %s",search_path);
        return 0;
    }
    if (!mod->readdir) return 0;
    size_t actual = mod->readdir(search_path, buf, size, offset);
    kprintf("Actual data %x",actual);
    return actual;
}

bool get_stat(const char *path, fs_stat *out_stat){
    if (!out_stat) return false;
    const char *search_path = path;
    if (*search_path == '/') search_path++;
    if (!*search_path){
        return stat_dir(out_stat);
    }
    system_module *mod = get_module(&search_path);
    if (!mod){
        kprintf("No module for path %s",search_path);
        return false;
    }
    if (!mod->getstat) return false;
    return mod->getstat(search_path, out_stat);
}

bool truncate(file *descriptor, size_t size){
    if (!open_files){
        kprintf("[FS] No open files");
        return false;
    }

    open_file_descriptors local = {};
    irq_flags_t irq = irq_save_disable();
    open_file_descriptors *ofile = (open_file_descriptors *)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (ofile) local = *ofile;
    irq_restore(irq);
    if (!ofile || !local.mod || !local.mod->truncate || local.pid != get_current_proc_pid()) return false;

    file gfd = (file){
        .id = local.mfile_id,
        .size = descriptor->size,
        .cursor = descriptor->cursor,
    };
    
    if (!local.mod->truncate(&gfd, size)) return false;

    descriptor->size = gfd.size;
    descriptor->cursor = gfd.cursor;
    if (descriptor->cursor > descriptor->size) descriptor->cursor = descriptor->size;

    irq = irq_save_disable();
    ofile = (open_file_descriptors *)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (ofile) ofile->file_size = gfd.size;
    irq_restore(irq);
    return true;
}

void close_files_for_process(uint16_t pid){
    if (open_files) {
        for (;;) {
            uint64_t fid = 0;
            bool found = false;
            for (uint64_t i = 0; i < open_files->capacity && !found; i++) {
                chashmap_entry_t *e = open_files->buckets[i];
                while (e) {
                    open_file_descriptors *f = (open_file_descriptors*)e->value;
                    if (f && f->pid == pid) {
                        fid = f->file_id;
                        found = true;
                        break;
                    }
                    e = e->next;
                }
            }
            if (!found) break;
            file fd = {};
            fd.id = fid;
            close_file(&fd);
        }
    }
    close_pipes_for_process(pid);
}

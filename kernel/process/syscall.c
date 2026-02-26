#include "syscall.h"
#include "console/kio.h"
#include "exceptions/exception_handler.h"
#include "console/serial/uart.h"
#include "exceptions/irq.h"
#include "mouse_input.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "memory/talloc.h"
#include "graph/graphics.h"
#include "std/memory_access.h"
#include "input/input_dispatch.h"
#include "std/memory.h"
#include "std/string.h"
#include "exceptions/timer.h"
#include "networking/network.h"
#include "networking/port_manager.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscall_codes.h"
#include "graph/tres.h"
#include "memory/mmu.h"
#include "loading/process_loader.h"
#include "networking/interface_manager.h"
#include "bin/bin_mod.h"
#include "networking/transport_layer/csocket.h"
#include "loading/dwarf.h"
#include "sysregs.h"
#include "memory/addr.h"
#include "ui/graphic_types.h"
#include "dev/module_loader.h"

int syscall_depth = 0;
uintptr_t cpec;

//TEST: What happens if we pass another process' data in here?
typedef uint64_t (*syscall_entry)(process_t *ctx);

static bool mm_try_handle_page_fault(process_t *proc, uintptr_t far, uint64_t esr) {
    if (!proc) return false;

    uint64_t ec = (esr >> 26) & 0x3F;
    uint32_t iss = esr & 0xFFFFFF;
    uint8_t ifsc = esr & 0x3F;

    bool is_exec = ec == 0x20;
    bool is_write = false;
    if (!is_exec) is_write = ((iss >> 6) & 1) != 0;

    if (ifsc >= 0x9 && ifsc <= 0xB) {
        if (!mmu_set_access_flag((uint64_t*)proc->ttbr, far)) return false;
        mmu_flush_asid(proc->asid);
        return true;
    }

    if (ifsc >= 0xD && ifsc <= 0xF) return false;
    if (ifsc < 0x4 || ifsc > 0x7) return false;

    uintptr_t va_page = far & ~(PAGE_SIZE - 1);
    vma *m = mm_find_vma(&proc->mm, va_page);
    if (!m) return false;

    if (is_exec) {
        if (!(m->prot & MEM_EXEC)) return false;
    } else if (is_write) {
        if (!(m->prot & MEM_RW)) return false;
    }

    if (!(m->flags & VMA_FLAG_DEMAND)) return false;

    if (m->kind == VMA_KIND_STACK) {
        if (va_page < proc->mm.stack_limit) return false;
        if (va_page >= proc->mm.stack_top) return false;

        if (va_page + PAGE_SIZE == proc->mm.stack_commit) {
            if (proc->mm.stack_commit <= proc->mm.stack_limit) return false;
            if (proc->mm.rss_stack_pages >= proc->mm.cap_stack_pages) return false;
            proc->mm.stack_commit -= PAGE_SIZE;
        } else if (va_page < proc->mm.stack_commit) {
            return false;
        } else {
            if (proc->mm.rss_stack_pages >= proc->mm.cap_stack_pages) return false;
        }
    } else if (m->kind == VMA_KIND_HEAP) {
        if (proc->mm.rss_heap_pages >= proc->mm.cap_heap_pages) return false;
    } else if (m->kind == VMA_KIND_ANON) {
        if (proc->mm.rss_anon_pages >= proc->mm.cap_anon_pages) return false;
    }

    paddr_t phys = palloc_inner(PAGE_SIZE, MEM_PRIV_USER, MEM_RW, true, false);
    if (!phys) return false;

    if (m->kind != VMA_KIND_ANON || (m->flags & VMA_FLAG_ZERO)) memset((void*)dmap_pa_to_kva(phys), 0, PAGE_SIZE);
    mmu_map_4kb((uint64_t*)proc->ttbr, va_page, phys, MAIR_IDX_NORMAL, m->prot | MEM_NORM, MEM_PRIV_USER);
    mmu_flush_asid(proc->asid);

    if (m->kind == VMA_KIND_STACK) proc->mm.rss_stack_pages++;
    else if (m->kind == VMA_KIND_HEAP) proc->mm.rss_heap_pages++;
    else if (m->kind == VMA_KIND_ANON) proc->mm.rss_anon_pages++;

    return true;
}

static bool uaccess_copyin(process_t *proc, void *dst, uintptr_t src, size_t size) {
    if (!size) return true;

    if ((src >> 47) & 1) {
        memcpy(dst, (const void*)src, size);
        return true;
    }

    uint8_t *d = (uint8_t*)dst;

    while (size) {
        size_t off = src & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - off;
        if (chunk > size) chunk = size;

        int st = 0;
        uintptr_t pa = mmu_translate(src, &st);
        if (st) {
            uint64_t esr = (0x24ULL << 26) | 0x7ULL;
            if (!mm_try_handle_page_fault(proc, src, esr)) return false;
            pa = mmu_translate(src, &st);
            if (st) return false;
        }

        memcpy(d, (const void*)PHYS_TO_VIRT_P((void*)pa), chunk);
        d += chunk;
        src += chunk;
        size -= chunk;
    }

    return true;
}

static bool uaccess_copyout(process_t *proc, uintptr_t dst, const void *src, size_t size) {
    if (!size) return true;

    if ((dst >> 47) & 1) {
        memcpy((void*)dst, src, size);
        return true;
    }

    const uint8_t *s = (const uint8_t*)src;

    while (size) {
        size_t off = dst & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - off;
        if (chunk > size) chunk = size;

        int st = 0;
        uintptr_t pa = mmu_translate(dst, &st);
        if (st){
            uint64_t esr = (0x24ULL << 26)| (1ULL << 6) | 0x7ULL;
            if (!mm_try_handle_page_fault(proc, dst, esr)) return false;
            pa = mmu_translate(dst, &st);
            if (st) return false;
        }

        memcpy((void*)PHYS_TO_VIRT_P((void*)pa), s, chunk);
        s += chunk;
        dst += chunk;
        size -= chunk;
    }

    return true;
}

static bool uaccess_copyinstr(process_t *proc, char *dst, size_t dst_size, uintptr_t src, size_t *out_copied, bool *out_terminated) {
    if (!dst || dst_size < 2) return false;

    size_t pos = 0;
    bool term = false;

    while (pos + 1 < dst_size) {
        uint8_t c = 0;
        if (!uaccess_copyin(proc, &c, src + pos, 1)) return false;
        dst[pos] = (char)c;
        pos++;
        if (!c) {
            term = true;
            break;
        }
    }

    if (!term) dst[dst_size - 1] = 0;

    if (out_copied) *out_copied = term ? pos : (dst_size - 1);
    if (out_terminated) *out_terminated = term;

    return true;
}

u64 syscall_malloc(process_t *ctx){
    if (syscall_depth > 1) {
        process_t *k = get_proc_by_pid(1);
        if (!k) return 0;
        return (u64)kalloc(PHYS_TO_VIRT_P((void*)k->heap_phys), ctx->PROC_X0, ALIGN_16B, MEM_PRIV_KERNEL);
    }

    size_t size = ctx->PROC_X0;
    if (!size) return 0;

    u64 pages = count_pages(size, PAGE_SIZE);
    size_t alloc_size = pages * PAGE_SIZE;
    if (ctx->mm.rss_anon_pages + pages > ctx->mm.cap_anon_pages) return 0;

    uptr va = mm_alloc_mmap(&ctx->mm, alloc_size, MEM_RW, VMA_KIND_ANON, VMA_FLAG_DEMAND | VMA_FLAG_USERALLOC | VMA_FLAG_ZERO);
    if (!va) return 0;

    mmu_flush_asid(ctx->asid);
    return va;
}

uptr syscall_palloc(process_t *ctx){
    size_t size = ctx->PROC_X0;
    if(!size) return 0;
    u64 pages = count_pages(size, PAGE_SIZE);
    size_t alloc_size = pages * PAGE_SIZE;

    if (ctx->use_va){
        if (ctx->mm.rss_anon_pages + pages > ctx->mm.cap_anon_pages) return 0;
        //TODO zalloc likely needs a dedicated syscall to request zeroed on fault explicitly
        uptr va = mm_alloc_mmap(&ctx->mm, alloc_size, MEM_RW, VMA_KIND_ANON, VMA_FLAG_DEMAND | VMA_FLAG_USERALLOC | VMA_FLAG_ZERO);
        if (!va) return 0;
        mmu_flush_asid(ctx->asid);
        return va;
    }

    paddr_t ptr = palloc_inner(alloc_size, MEM_PRIV_USER, MEM_RW, true, true);
    if(!ptr) return 0;
    register_allocation(ctx->alloc_map, (void*)ptr, alloc_size);
    mmu_flush_asid(ctx->asid);
    return (uptr)dmap_pa_to_kva(ptr);
}

u64 syscall_pfree(process_t *ctx){
    uptr va = ctx->PROC_X0;
    if (!va) return 0;

    if (ctx->use_va) {
        vma *m = mm_find_vma(&ctx->mm,va);
        if (!m) return 0;
        if (m->kind != VMA_KIND_ANON) return 0;
        if (!(m->flags & VMA_FLAG_USERALLOC)) return 0;

        uintptr_t start = m->start;
        uintptr_t end = m->end;
        if (!mm_remove_vma(&ctx->mm, start, end)) return 0;

        for (uintptr_t a = start; a < end; a += PAGE_SIZE) {
            uint64_t pa = 0;
            if (!mmu_unmap_and_get_pa((uint64_t*)ctx->ttbr, a, &pa)) continue;
            pfree((void*)pa, PAGE_SIZE);
            if (ctx->mm.rss_anon_pages) ctx->mm.rss_anon_pages--;
        }

        mmu_flush_asid(ctx->asid);
        return 0;
    }

    int tr = 0;
    uptr phys = mmu_translate(va, &tr);
    if (tr) return 0;

    for (page_index *ind = ctx->alloc_map; ind; ind = ind->header.next) {
        for (u64 i = 0; i < ind->header.size; i++) {
            if ((uptr)ind->ptrs[i].ptr == phys) {
                size_t size = ind->ptrs[i].size;
                u64 pages = count_pages(size, PAGE_SIZE);
                for (u64 p = 0; p < pages; p++) mmu_unmap_table(ctx->ttbr, va + (p * PAGE_SIZE), phys + (p * PAGE_SIZE));

                mmu_flush_asid(ctx->asid);
                pfree((void*)phys, size);
                if (ctx->mm.rss_anon_pages >= pages) ctx->mm.rss_anon_pages -= pages;
                else ctx->mm.rss_anon_pages = 0;

                ind->ptrs[i] = ind->ptrs[ind->header.size - 1];
                ind->header.size--;
                return 0;
            }
        }
    }
    return 0;
}

u64 syscall_free(process_t *ctx){
    kfree((void*)ctx->PROC_X0, ctx->PROC_X1);
    return 0;
}

uptr syscall_brk(process_t *ctx) {
    uptr req = ctx->PROC_X0;
    uptr old = ctx->mm.brk;
    if (!req) return old;

    uptr new_brk = (req + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uptr min_brk = ctx->mm.heap_start + PAGE_SIZE;
    if (new_brk < min_brk) new_brk = min_brk;

    if (new_brk > ctx->mm.brk_max) return old;

    uptr mmap_guard = ctx->mm.mmap_cursor - (MM_GAP_PAGES*PAGE_SIZE);
    if (new_brk > mmap_guard)return old;

    if (new_brk < old) {
        for (uptr va = new_brk; va < old; va += PAGE_SIZE) {
            uint64_t pa = 0;
            if (!mmu_unmap_and_get_pa((uint64_t*)ctx->ttbr, va, &pa)) continue;
            pfree((void*)pa, PAGE_SIZE);
            if (ctx->mm.rss_heap_pages) ctx->mm.rss_heap_pages--;
        }
    }

    if (new_brk != old) {
        ctx->mm.brk = new_brk;
        mm_update_vma(&ctx->mm, ctx->mm.heap_start, ctx->mm.brk);
        mmu_flush_asid(ctx->asid);
    }

    return ctx->mm.brk;
}

u64 syscall_printl(process_t *ctx){
    uintptr_t u = (uintptr_t)ctx->PROC_X0;
    if (!u) return 0;

    char buf[256] = {};

    for (;;) {
        size_t copied = 0;
        bool term = false;
        if (!uaccess_copyinstr(ctx, buf, sizeof(buf), u, &copied, &term)) return 0;
        kprint(buf);
        if (term) break;
        if (!copied) break;
        u += copied;
    }

    return 0;
}

u64 syscall_read_key(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    keypress tmp = {};
    u64 r = sys_read_input_current(&tmp);
    if (!uaccess_copyout(ctx, up, &tmp, sizeof(tmp))) return 0;
    return r;
}

u64 syscall_read_event(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    kbd_event tmp = {};
    u64 r = sys_read_event_current(&tmp);
    if (!uaccess_copyout(ctx, up, &tmp, sizeof(tmp))) return 0;
    return r;
}

u64 syscall_read_shortcut(process_t *ctx){
    kprint("[SYSCALL implementation error] Shortcut syscalls are not implemented yet");
    return 0;
}

u64 syscall_get_mouse(process_t *ctx){
    //TEST: are we preventing the mouse from being read outside of window?
    if (get_current_proc_pid() != ctx->id) return 0;
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    mouse_data tmp = {};
    tmp.raw = get_raw_mouse_in();
    tmp.position = convert_mouse_position(get_mouse_pos());
    if (!uaccess_copyout(ctx, up, &tmp, sizeof(tmp))) return 0;
    return 0;
}

uptr syscall_gpu_request_ctx(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    draw_ctx tmp = {};
    get_window_ctx(&tmp);
    if (!uaccess_copyout(ctx, up, &tmp, sizeof(tmp))) return 0;
    return 0;
}

u64 syscall_gpu_flush(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    draw_ctx tmp = {};
    if (!uaccess_copyin(ctx, &tmp, up, sizeof(tmp))) return 0;
    commit_frame(&tmp, 0);
    gpu_flush();
    return 0;
}

u64 syscall_gpu_resize_ctx(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    uint32_t width = (uint32_t)ctx->PROC_X1;
    uint32_t height = (uint32_t)ctx->PROC_X2;
    resize_window(width, height);
    draw_ctx tmp = {};
    get_window_ctx(&tmp);
    if (!uaccess_copyout(ctx, up, &tmp, sizeof(tmp))) return 0;
    gpu_flush();
    return 0;
}

u64 syscall_char_size(process_t *ctx){
    return gpu_get_char_size(ctx->PROC_X0);
}

u64 syscall_msleep(process_t *ctx){
    syscall_depth--;
    sleep_process(ctx->PROC_X0);
    return 0;
}

u64 syscall_halt(process_t *ctx){
    kprintf("Process has ended with code %i",ctx->PROC_X0);
    syscall_depth--;
    stop_current_process(ctx->PROC_X0);
    return 0;
}

u64 syscall_exec(process_t *ctx){
    const char *prog_name = (const char*)ctx->PROC_X0;
    int argc = ctx->PROC_X1;
    const char **argv = (const char**)ctx->PROC_X2;
    process_t *p = execute(prog_name, argc, argv);
    if (p) p->win_id = ctx->win_id;
    return p ? p->id : 0;
}

u64 syscall_get_time(process_t *ctx){
    return timer_now_msec();
}

u64 syscall_socket_create(process_t *ctx){
    Socket_Role role = (Socket_Role)ctx->PROC_X0;
    protocol_t protocol = (protocol_t)ctx->PROC_X1;
    uintptr_t uextra = (uintptr_t)ctx->PROC_X2;
    uintptr_t uout = (uintptr_t)ctx->PROC_X3;
    SocketExtraOptions extra = {};
    SocketExtraOptions *pe = 0;
    if (uextra) {
        if (!uaccess_copyin(ctx, &extra, uextra, sizeof(extra))) return 0;
        pe = &extra;
    }

    SocketHandle out = {};
    u64 r = create_socket(role, protocol, pe, ctx->id, &out);
    if (!uaccess_copyout(ctx, uout, &out, sizeof(out))) return 0;
    return r;
}

u64 syscall_socket_bind(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    ip_version_t ip_version = (ip_version_t)ctx->PROC_X1;
    uint16_t port = (uint16_t)ctx->PROC_X2;
    SocketHandle handle = {};
    if (!uaccess_copyin(ctx, &handle, uhandle, sizeof(handle))) return 0;
    return bind_socket(&handle, port, ip_version, ctx->id);
}

u64 syscall_socket_connect(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    uint8_t dst_kind = (uint8_t)ctx->PROC_X1;
    uintptr_t udst = (uintptr_t)ctx->PROC_X2;
    uint16_t port = (uint16_t)ctx->PROC_X3;

    SocketHandle handle = {};
    if (!uaccess_copyin(ctx, &handle, uhandle, sizeof(handle))) return 0;

    net_l4_endpoint ep = {};
    char domain[256] = {};
    const void *dst = 0;

    if (dst_kind == DST_ENDPOINT) {
        if (!uaccess_copyin(ctx, &ep, udst, sizeof(ep))) return 0;
        dst = &ep;
    } else if (dst_kind == DST_DOMAIN) {
        size_t copied = 0;
        bool term = false;
        if (!uaccess_copyinstr(ctx, domain, sizeof(domain), udst, &copied, &term)) return 0;
        if (!term) return 0;
        dst = domain;
    } else {
        return 0;
    }

    return connect_socket(&handle, dst_kind, dst, port, ctx->id);
}

u64 syscall_socket_listen(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    int32_t backlog = (int32_t)ctx->PROC_X1;

    SocketHandle handle = {};
    if (!uaccess_copyin(ctx, &handle, uhandle, sizeof(handle))) return 0;
    return listen_on(&handle, backlog, ctx->id);
}

u64 syscall_socket_accept(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    SocketHandle handle = {};
    if (!uaccess_copyin(ctx, &handle, uhandle, sizeof(handle))) return 0;
    accept_on_socket(&handle, ctx->id);
    return 1;
}

u64 syscall_socket_send(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    uint8_t dst_kind = (uint8_t)ctx->PROC_X1;
    uintptr_t udst = (uintptr_t)ctx->PROC_X2;
    uint16_t port = (uint16_t)ctx->PROC_X3;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X4;
    size_t size = (size_t)ctx->regs[5];

    SocketHandle handle = {};
    if (!uaccess_copyin(ctx, &handle, uhandle, sizeof(handle))) return 0;

    net_l4_endpoint ep = {};
    char domain[256] = {};
    const void *dst = 0;

    if (dst_kind == DST_ENDPOINT){
        if (!uaccess_copyin(ctx, &ep, udst, sizeof(ep))) return 0;
        dst = &ep;
    } else if (dst_kind == DST_DOMAIN){
        size_t copied = 0;
        bool term = false;
        if (!uaccess_copyinstr(ctx, domain, sizeof(domain), udst, &copied, &term)) return 0;
        if (!term) return 0;
        dst = domain;
    } else {
        return 0;
    }

    if (!size) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = (void*)talloc(alloc_size);
    if (!kbuf) return 0;

    if (!uaccess_copyin(ctx, kbuf, ubuf, size)){
        temp_free(kbuf, alloc_size);
        return 0;
    }

    u64 r = send_on_socket(&handle, dst_kind, dst, port, kbuf, size, ctx->id);
    temp_free(kbuf, alloc_size);
    return r;
}

u64 syscall_socket_receive(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    uintptr_t uout_src = (uintptr_t)ctx->PROC_X3;

    SocketHandle handle = {};
    if (!uaccess_copyin(ctx, &handle, uhandle, sizeof(handle))) return 0;
    if (!size) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = (void*)talloc(alloc_size);
    if (!kbuf) return 0;

    net_l4_endpoint src = {};
    int64_t r = receive_from_socket(&handle, kbuf, size, &src, ctx->id);
    if (r > 0) {
        if (!uaccess_copyout(ctx, ubuf, kbuf, (size_t)r)) r = 0;
    }
    if (!uaccess_copyout(ctx, uout_src, &src, sizeof(src))) r = 0;

    temp_free(kbuf, alloc_size);
    return (u64)r;
}

u64 syscall_socket_close(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    SocketHandle handle = {};
    if (!uaccess_copyin(ctx, &handle, uhandle, sizeof(handle))) return 0;
    return close_socket(&handle, ctx->id);
}

u64 syscall_openf(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t udesc = (uintptr_t)ctx->PROC_X1;

    char req_path[255] = {};
    size_t copied = 0;
    bool term = false;
    if (!uaccess_copyinstr(ctx, req_path, sizeof(req_path), upath, &copied, &term)) return FS_RESULT_DRIVER_ERROR;
    if (!term) return FS_RESULT_DRIVER_ERROR;

    char path[255] = {};
    if (!(ctx->PROC_PRIV) && strstart_case("/resources/", req_path, true) == 11){
        string_format_buf(path, sizeof(path), "%s%s", ctx->bundle, req_path);
    } else {
        memcpy(path, req_path, strlen(req_path) + 1);
    }
    //TODO: Restrict access to own bundle, own fs and require privilege escalation for full-ish filesystem access
    file descriptor = {};
    FS_RESULT r = open_file(path, &descriptor);
    if (!uaccess_copyout(ctx, udesc, &descriptor, sizeof(descriptor))) return FS_RESULT_DRIVER_ERROR;
    return r;
}

u64 syscall_readf(process_t *ctx){
    uintptr_t udesc = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    file descriptor = {};
    if (!uaccess_copyin(ctx, &descriptor, udesc, sizeof(descriptor))) return 0;

    uint8_t tmp[4096];
    size_t done = 0;

    while (done < size){
        size_t chunk = size - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);

        size_t r = read_file(&descriptor, (char*)tmp, chunk);
        if (!r) break;

        if (!uaccess_copyout(ctx, ubuf + done, tmp, r)) break;

        done += r;
        if (r < chunk) break;
    }

    uaccess_copyout(ctx, udesc, &descriptor, sizeof(descriptor));
    return done;
}

u64 syscall_writef(process_t *ctx){
    uintptr_t udesc = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;

    file descriptor = {};
    if (!uaccess_copyin(ctx, &descriptor, udesc, sizeof(descriptor))) return 0;

    uint8_t tmp[4096];
    size_t done = 0;

    while (done < size) {
        size_t chunk = size - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);

        if (!uaccess_copyin(ctx, tmp, ubuf + done, chunk)) break;

        size_t w = write_file(&descriptor, (const char*)tmp, chunk);
        if (!w) break;

        done += w;
        if (w < chunk) break;
    }

    uaccess_copyout(ctx, udesc, &descriptor, sizeof(descriptor));
    return done;
}

u64 syscall_sreadf(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;

    char path[255] = {};
    size_t copied = 0;
    bool term = false;
    if (!uaccess_copyinstr(ctx, path, sizeof(path), upath, &copied, &term)) return 0;
    if (!term) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = (void*)talloc(alloc_size);
    if (!kbuf) return 0;

    size_t r = simple_read(path, kbuf, size);
    if (r) uaccess_copyout(ctx, ubuf, kbuf, r);

    temp_free(kbuf, alloc_size);
    return r;
}

u64 syscall_swritef(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;

    char path[255] = {};
    size_t copied = 0;
    bool term = false;
    if (!uaccess_copyinstr(ctx, path, sizeof(path), upath, &copied, &term)) return 0;
    if (!term) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = (void*)talloc(alloc_size);
    if (!kbuf) return 0;

    if (!uaccess_copyin(ctx, kbuf, ubuf, size)){
        temp_free(kbuf, alloc_size);
        return 0;
    }

    size_t r = simple_write(path, kbuf, size);
    temp_free(kbuf, alloc_size);
    return r;
}

u64 syscall_closef(process_t *ctx){
    uintptr_t udesc = (uintptr_t)ctx->PROC_X0;
    file descriptor = {};
    if (!uaccess_copyin(ctx, &descriptor, udesc, sizeof(descriptor))) return 0;
    close_file(&descriptor);
    return 0;
}

u64 syscall_dir_list(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    uintptr_t uoffset = (uintptr_t)ctx->PROC_X3;

    char path[255] = {};
    size_t copied = 0;
    bool term = false;
    if (!uaccess_copyinstr(ctx, path, sizeof(path), upath, &copied, &term)) return 0;
    if (!term) return 0;

    uint64_t off = 0;
    if (!uaccess_copyin(ctx, &off, uoffset, sizeof(off))) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = (void*)talloc(alloc_size);
    if (!kbuf) return 0;

    size_t r = list_directory_contents(path, kbuf, size, &off);
    if (r) uaccess_copyout(ctx, ubuf, kbuf, r);
    uaccess_copyout(ctx, uoffset, &off, sizeof(off));

    temp_free(kbuf, alloc_size);
    return r;
}

// uint64_t syscall_load_fsmod(process_t *ctx){
//     system_module *mod = (system_module*)ctx->PROC_X0;
//     return load_process_module(ctx,mod);
// }

// uint64_t syscall_unload_fsmod(process_t *ctx){
//     return unload_module(&ctx->exposed_fs);
// }

u64 syscall_in_case_of_js(process_t *ctx){
    panic("Shame on you\r\n\
Don't ever do that again\r\n\
....................../´¯/) \r\n\
....................,/¯../ \r\n\
.................../..../ \r\n\
............./´¯/'...'/´¯¯`·¸ \r\n\
........../'/.../..../......./¨¯\\\r\n\
........('(...´...´.... ¯~/'...') \r\n\
.........\\.................'...../\r\n\
..........''...\\.......... _.·´\r\n\
............\\..............(\r\n\
..............\\.............\\...",0);
    return 0;
}

syscall_entry syscalls[] = {
    [MALLOC_CODE] = syscall_malloc,
    [FREE_CODE] = syscall_free,
    [BRK_CODE] = syscall_brk,
    [PALLOC_CODE] = syscall_palloc,
    [PFREE_CODE] = syscall_pfree,
    [PRINTL_CODE] = syscall_printl,
    [READ_KEY_CODE] = syscall_read_key,
    [READ_EVENT_CODE] = syscall_read_event,
    [READ_SHORTCUT_CODE] = syscall_read_shortcut,
    [GET_MOUSE_STATUS_CODE] = syscall_get_mouse,
    [REQUEST_DRAW_CTX_CODE] = syscall_gpu_request_ctx,
    [GPU_FLUSH_DATA_CODE] = syscall_gpu_flush,
    [GPU_CHAR_SIZE_CODE] = syscall_char_size,
    [RESIZE_DRAW_CTX_CODE] = syscall_gpu_resize_ctx,
    [SLEEP_CODE] = syscall_msleep,
    [HALT_CODE] = syscall_halt,
    [EXEC_CODE] = syscall_exec,
    [GET_TIME_CODE] = syscall_get_time,
    [SOCKET_CREATE_CODE] = syscall_socket_create,
    [SOCKET_BIND_CODE] = syscall_socket_bind,
    [SOCKET_CONNECT_CODE] = syscall_socket_connect,
    [SOCKET_LISTEN_CODE] = syscall_socket_listen,
    [SOCKET_ACCEPT_CODE] = syscall_socket_accept,
    [SOCKET_SEND_CODE] = syscall_socket_send,
    [SOCKET_RECEIVE_CODE] = syscall_socket_receive,
    [SOCKET_CLOSE_CODE] = syscall_socket_close,
    [FILE_OPEN_CODE] = syscall_openf,
    [FILE_READ_CODE] = syscall_readf,
    [FILE_WRITE_CODE] = syscall_writef,
    [FILE_CLOSE_CODE] = syscall_closef,
    [FILE_SIMPLE_READ_CODE] = syscall_sreadf,
    [FILE_SIMPLE_WRITE_CODE] = syscall_swritef,
    [DIR_LIST_CODE] = syscall_dir_list,
    // [LOAD_FSMODULE_CODE] = syscall_load_fsmod,
    // [UNLOAD_FSMODULE_CODE] = syscall_unload_fsmod,
    [IN_CASE_OF_JS_CODE] = syscall_in_case_of_js,
};

bool decode_crash_address_with_info(uint8_t depth, uintptr_t address, sizedptr debug_line, sizedptr debug_line_str){
    if (!debug_line.ptr || !debug_line.size) return false;
    debug_line_info info = dwarf_decode_lines(debug_line.ptr, debug_line.size, debug_line_str.ptr, debug_line_str.size, VIRT_TO_PHYS(address));
    if (info.address == VIRT_TO_PHYS(address)){
        kprintf("[%.16x] %i: %s %i:%i", address, depth, info.file, info.line, info.column);
        return true;
    }
    return false;
}

bool decode_crash_address(uint8_t depth, uintptr_t address, sizedptr debug_line, sizedptr debug_line_str){
    return decode_crash_address_with_info(depth, address, debug_line, debug_line_str) ||
    decode_crash_address_with_info(depth, address, get_proc_by_pid(0)->debug_lines, get_proc_by_pid(0)->debug_line_str);
}

void backtrace(uintptr_t fp, uintptr_t elr, sizedptr debug_line, sizedptr debug_line_str) {

    if (elr){
        if (!decode_crash_address(0, elr, debug_line, debug_line_str))
            kprintf("Exception triggered by %llx",(elr));
    }

    for (uint8_t depth = 1; depth < 10 && fp; depth++) {
        int tr_ra = 0;
        uintptr_t ra_pa = mmu_translate(fp + 8, &tr_ra);
        if (tr_ra) return;

        uintptr_t return_address = (*(uintptr_t*)PHYS_TO_VIRT_P(ra_pa));
        if (!return_address) return;
        return_address -= 4;//Return address is the next instruction after branching
        if (!decode_crash_address(depth, return_address, debug_line, debug_line_str))
            kprintf("%i: caller address: %llx", depth, return_address, return_address);
        int tr = 0;
        uintptr_t fp_pa = mmu_translate(fp, &tr);
        if (tr) return;
        fp = *(uintptr_t*)PHYS_TO_VIRT_P(fp_pa);
    }
}

const char* fault_messages[] = {
    [0b000000] = "Address size fault in TTBR0 or TTBR1",
    [0b000100] = "Translation fault, 0th level",
    [0b000101] = "Translation fault, 1st level",
    [0b000110] = "Translation fault, 2nd level",
    [0b000111] = "Translation fault, 3rd level",
    [0b001001] = "Access flag fault, 1st level",
    [0b001010] = "Access flag fault, 2nd level",
    [0b001011] = "Access flag fault, 3rd level",
    [0b001101] = "Permission fault, 1st level",
    [0b001110] = "Permission fault, 2nd level",
    [0b001111] = "Permission fault, 3rd level",
    [0b010000] = "Synchronous external abort",
    [0b011000] = "Synchronous parity error on memory access",
    [0b010101] = "Synchronous external abort on translation table walk, 1st level",
    [0b010110] = "Synchronous external abort on translation table walk, 2nd level",
    [0b010111] = "Synchronous external abort on translation table walk, 3rd level",
    [0b011101] = "Synchronous parity error on memory access on translation table walk, 1st level",
    [0b011110] = "Synchronous parity error on memory access on translation table walk, 2nd level",
    [0b011111] = "Synchronous parity error on memory access on translation table walk, 3rd level",
    [0b100001] = "Alignment fault",
    [0b100010] = "Debug event",
};

void coredump(uintptr_t esr, uintptr_t elr, uintptr_t far, uintptr_t sp){
    uint8_t ifsc = esr & 0x3F;

    const char *m = 0;
    if (ifsc < 64) m = fault_messages[ifsc];
    if (!m) m = "Unknown fault";
    uint64_t sctlr = 0;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if (sctlr & 1) m = (const char*)(((uintptr_t)m) | HIGH_VA);
    kprint(m);
    process_t *proc = get_current_proc();
    backtrace(sp, elr, proc->debug_lines, proc->debug_line_str);

    // for (int i = 0; i < 31; i++)
    //     kprintf("Reg[%i - %x] = %x",i,&proc->regs[i],proc->regs[i]);
    if (far > 0)
        debug_mmu_address(far);
    else
        kprintf("Null pointer accessed at %x",elr);
}

void sync_el0_handler_c(){
    save_return_address_interrupt();

    syscall_depth++;
#if TEST
    if (syscall_depth > 10 || syscall_depth < 0) panic("Too much syscall recursion", syscall_depth);
#endif

    process_t *proc = get_current_proc();

    uint64_t x0 = proc->PROC_X0;
    uint64_t elr;
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    uint64_t spsr;
    asm volatile ("mrs %0, spsr_el1" : "=r"(spsr));

    uint64_t currentEL = (spsr >> 2) & 3;

    uint64_t esr;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));

    uint64_t ec = (esr >> 26) & 0x3F;
    uint64_t iss = esr & 0xFFFFFF;

    uint64_t far;
    asm volatile ("mrs %0, far_el1" : "=r"(far));
    if (ec == 0x24 || ec == 0x20){
        if (mm_try_handle_page_fault(proc, far, esr)){
            syscall_depth--;
            process_restore();
        }
    }

    uint64_t result = 0;
    if (ec == 0x15) {
        syscall_entry entry = syscalls[iss];
        if (entry){
            uint64_t sctlr = 0;
            asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
            if (sctlr & 1) entry = (syscall_entry)(((uintptr_t)entry) | HIGH_VA);
            result = entry(proc);
        } else {
            kprintf("Unknown syscall in process. ESR: %x. ELR: %x. FAR: %x", esr, elr, far);
            coredump(esr, elr, far, proc->sp);
            syscall_depth--;
            stop_current_process(ec);
        }
    } else {
        if (far == 0 && elr == 0 && currentEL == 0){
            kprintf("Process has exited %x",x0);
            syscall_depth--;
            stop_current_process(x0);
        } else {
            switch (ec) {
                case 0x20:
                case 0x21: {
                    if (far == 0){
                        kprintf("Process has exited %x",x0);
                        syscall_depth--;
                        stop_current_process(x0);
                    }
                }
            }
            if (currentEL == 1){
                if (syscall_depth < 3){
                    if (syscall_depth < 1) kprintf("System has crashed. ESR: %llx. ELR: %llx. FAR: %llx", esr, elr, far);
                    if (syscall_depth < 2) coredump(esr, elr, far, proc->sp);
                    handle_exception("UNEXPECTED EXCEPTION",ec);
                }
                while (true);
            } else {
                kprintf("Process has crashed. ESR: %llx. ELR: %llx. FAR: %llx. SP: %llx", esr, elr, far, proc->sp);
                if (syscall_depth < 2) coredump(esr, elr, far, proc->sp);
                syscall_depth--;
                stop_current_process(ec);
            }
        }
    }
    syscall_depth--;
    save_syscall_return(result);
    process_restore();
}

void trace(){
    uint64_t elr;
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    uint64_t esr;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    uint64_t far;
    asm volatile ("mrs %0, far_el1" : "=r"(far));
    uint64_t sp;
    asm volatile ("mov %0, sp" : "=r"(sp));
    backtrace(sp, elr, (sizedptr){0,0}, (sizedptr){0,0});
}

#include "syscall.h"
#include "console/kio.h"
#include "exceptions/exception_handler.h"
#include "console/serial/uart.h"
#include "exceptions/irq.h"
#include "mouse_input.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
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
#include "process/loading/elf_file.h"
#include "networking/interface_manager.h"
#include "bin/bin_mod.h"
#include "networking/transport_layer/csocket.h"
#include "loading/dwarf.h"
#include "sysregs.h"
#include "memory/addr.h"
#include "ui/graphic_types.h"
#include "dev/module_loader.h"
#include "alloc/page_index.h"
#include "process/uaccess.h"

int syscall_depth = 0;
uintptr_t cpec;

//TEST: What happens if we pass another process' data in here?
typedef uint64_t (*syscall_entry)(process_t *ctx);

u64 syscall_malloc(process_t *ctx){
    if (syscall_depth > 1) {
        process_t *k = get_kernel_proc();
        if (!k) return 0;
        return (u64)kalloc((void*)dmap_pa_to_kva((paddr_t)k->heap_phys), ctx->PROC_X0, ALIGN_16B, MEM_PRIV_KERNEL);
    }

    size_t size = ctx->PROC_X0;
    if (!size) return 0;

    u64 pages = count_pages(size, PAGE_SIZE);
    size_t alloc_size = pages * PAGE_SIZE;
    if (ctx->mm.rss_anon_pages + pages > ctx->mm.cap_anon_pages) return 0;

    uptr va = mm_alloc_mmap(&ctx->mm, alloc_size, MEM_RW, VMA_KIND_ANON, VMA_FLAG_DEMAND | VMA_FLAG_USERALLOC | VMA_FLAG_ZERO);
    if (!va) return 0;

    mmu_flush_asid(ctx->mm.asid);
    return va;
}

uptr syscall_palloc(process_t *ctx){
    size_t size = ctx->PROC_X0;
    if(!size) return 0;
    u64 pages = count_pages(size, PAGE_SIZE);
    size_t alloc_size = pages * PAGE_SIZE;

    if (ctx->mm.ttbr0){
        if (ctx->mm.rss_anon_pages + pages > ctx->mm.cap_anon_pages) return 0;
        //TODO zalloc likely needs a dedicated syscall to request zeroed on fault explicitly
        uptr va = mm_alloc_mmap(&ctx->mm, alloc_size, MEM_RW, VMA_KIND_ANON, VMA_FLAG_DEMAND | VMA_FLAG_USERALLOC | VMA_FLAG_ZERO);
        if (!va) return 0;
        mmu_flush_asid(ctx->mm.asid);
        return va;
    }

    paddr_t ptr = palloc_inner(alloc_size, MEM_PRIV_USER, MEM_RW, true, true);
    if(!ptr) return 0;
    void *kva = (void*)dmap_pa_to_kva(ptr);
    register_allocation(ctx->alloc_map, kva, alloc_size);
    return (uptr)kva;
}

u64 syscall_pfree(process_t *ctx){
    uptr va = ctx->PROC_X0;
    if (!va) return 0;

    if (ctx->mm.ttbr0) {
        vma *m = mm_find_vma(&ctx->mm,va);
        if (!m) return 0;
        if (m->kind != VMA_KIND_ANON) return 0;
        if (!(m->flags & VMA_FLAG_USERALLOC)) return 0;

        uintptr_t start = m->start;
        uintptr_t end = m->end;
        if (!mm_remove_vma(&ctx->mm, start, end)) return 0;

        for (uintptr_t a = start; a < end; a += PAGE_SIZE) {
            uint64_t pa = 0;
            if (!mmu_unmap_and_get_pa((uint64_t*)ctx->mm.ttbr0, a, &pa)) continue;
            pfree((void*)dmap_pa_to_kva((paddr_t)pa), PAGE_SIZE);
            if (ctx->mm.rss_anon_pages) ctx->mm.rss_anon_pages--;
        }

        mmu_flush_asid(ctx->mm.asid);
        return 0;
    }

    size_t size = get_alloc_size(ctx->alloc_map, (void*)va);
    if (!size) return 0;
    u64 pages = count_pages(size, PAGE_SIZE);
    free_registered(ctx->alloc_map, (void*)va);
    if (ctx->mm.rss_anon_pages >= pages) ctx->mm.rss_anon_pages -= pages;
    else ctx->mm.rss_anon_pages = 0;
    return 0;
}

u64 syscall_free(process_t *ctx){
    if (ctx->mm.ttbr0) return syscall_pfree(ctx);

    void *ptr = (void*)ctx->PROC_X0;
    size_t size = (size_t)ctx->PROC_X1;
    if (!ptr || !size) return 0;
    if (!ctx->alloc_map) return 0;

    size_t alloc_size = get_alloc_size(ctx->alloc_map, ptr);
    if (!alloc_size) return 0;
    if (size && alloc_size != size) return 0;

    free_registered(ctx->alloc_map, ptr);
    return 0;
}

u64 syscall_printl(process_t *ctx){
    uintptr_t u = (uintptr_t)ctx->PROC_X0;
    if (!u) return 0;

    char buf[256] = {};

    for (;;) {
        size_t copied = 0;
        bool term = false;
        uaccess_result_t ur = copy_str_from_user(ctx, buf, sizeof(buf), u, &copied, &term);
        if (ur != UACCESS_OK && ur != UACCESS_ENAMETOOLONG) return 0;
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
    if (!r) return 0;
    uaccess_result_t ur = copy_to_user(ctx, up, &tmp, sizeof(tmp));
    if (ur != UACCESS_OK) return 0;
    return r;
}

u64 syscall_read_event(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    kbd_event tmp = {};
    u64 r = sys_read_event_current(&tmp);
    if (!r) return 0;
    uaccess_result_t ur = copy_to_user(ctx, up, &tmp, sizeof(tmp));
    if (ur != UACCESS_OK) return 0;
    return r;
}

u64 syscall_read_shortcut(process_t *ctx){
    kprint("[SYSCALL implementation error] Shortcut syscalls are not implemented yet");
    return 0;
}

u64 syscall_get_mouse(process_t *ctx){
    //TODO: we're not fully preventing the mouse from being read outside of proc's window (raw & buttons)
    if (get_current_proc_pid() != ctx->id) return 0;
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    mouse_data tmp = {};
    tmp.raw = get_raw_mouse_in();
    tmp.raw.scroll = sys_read_scroll_current();
    tmp.position = convert_mouse_position(get_mouse_pos());
    uaccess_result_t ur = copy_to_user(ctx, up, &tmp, sizeof(tmp));
    if (ur != UACCESS_OK) return 0;
    return 0;
}

uptr syscall_gpu_request_ctx(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    draw_ctx tmp = {};
    get_window_ctx(&tmp);
    uaccess_result_t ur = copy_to_user(ctx, up, &tmp, sizeof(tmp));
    if (ur != UACCESS_OK) return 0;
    return 0;
}

u64 syscall_gpu_flush(process_t *ctx){
    uintptr_t up = (uintptr_t)ctx->PROC_X0;
    draw_ctx tmp = {};
    uaccess_result_t ur = copy_from_user(ctx, &tmp, up, sizeof(tmp));
    if (ur != UACCESS_OK) return 0;

    draw_ctx win = {};
    get_window_ctx(&win);
    if (!tmp.full_redraw) {
        if (tmp.dirty_count > MAX_DIRTY_RECTS) return 0;
        if (!win.width || !win.height) return 0;
        for (uint32_t i = 0; i < tmp.dirty_count; i++) {
            gpu_rect r = tmp.dirty_rects[i];
            if (r.point.x < 0 || r.point.y < 0) return 0;
            if ((uint32_t)r.point.x >= win.width || (uint32_t)r.point.y >= win.height) return 0;
            if (r.size.width > win.width - (uint32_t)r.point.x) return 0;
            if (r.size.height > win.height - (uint32_t)r.point.y) return 0;
        }
    }

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
    uaccess_result_t ur = copy_to_user(ctx, up, &tmp, sizeof(tmp));
    if (ur != UACCESS_OK) return 0;
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

//TODO exec now has an argument to decide whether the spawned proc should take control of input or whether the caller should keep it
//rn this is the cleanest way to make launch policy explicit, avoiding that a terminal attached child could steal input focus from the caller, waiting for return after spawn or hardcoding the policy by process name
//the more standard design would be to handle this through a proper control terminal model later instead of deciding focus in exec
//https://pubs.opengroup.org/onlinepubs/9699919799.orig/basedefs/V1_chap11.html
//https://pubs.opengroup.org/onlinepubs/007904975/functions/tcsetpgrp.html
//https://man7.org/linux/man-pages/man7/credentials.7.html
///https://en.wikipedia.org/wiki/Job_control_(Unix)
u64 syscall_exec(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    int argc = (int)ctx->PROC_X1;
    uintptr_t uargv = (uintptr_t)ctx->PROC_X2;
    uint32_t mode = (uint32_t)ctx->PROC_X3;

    if (argc < 0) return 0;

    char prog_name[256] = {};
    size_t copied = 0;
    bool term = false;
    uaccess_result_t ur = copy_str_from_user(ctx, prog_name, sizeof(prog_name), upath, &copied, &term);
    if (ur != UACCESS_OK) return 0;
    if (!term) return 0;

    const int max_args = 64;
    if (argc > max_args) return 0;

    user_argv_t user_argv = {};

    ur = copy_argv_from_user(ctx, argc, uargv, &user_argv);
    if (ur != UACCESS_OK) return 0;

    process_t *p = execute(prog_name, argc, user_argv.argv, mode);

    free_argv_from_user(&user_argv);
    return p ? p->id : 0;
}

u64 syscall_kill_process(process_t *ctx) {
    uint16_t pid = (uint16_t)ctx->PROC_X0;
    if (!pid) return 0;

    process_t *target = get_proc_by_pid(pid);
    if (!target || target->state == STOPPED) return 0;
    if (target->id == 1) return 0;
    if ((target->spsr & 0xF) != 0) return 0;
    if (!ctx->win_id || target->win_id != ctx->win_id) return 0;

    stop_process(pid, -9);
    return 0;
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
        uaccess_result_t ur = copy_from_user(ctx, &extra, uextra, sizeof(extra));
        if (ur != UACCESS_OK) return 0;
        pe = &extra;
    }

    SocketHandle out = {};
    i64 r = create_socket(role, protocol, pe, ctx->id, &out);
    if (r < 0) return r;

    uaccess_result_t ur = copy_to_user(ctx, uout, &out, sizeof(out));
    if (ur != UACCESS_OK) return 0;
    return r;
}

u64 syscall_socket_bind(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    ip_version_t ip_version = (ip_version_t)ctx->PROC_X1;
    uint16_t port = (uint16_t)ctx->PROC_X2;
    SocketHandle handle = {};
    uaccess_result_t ur = copy_from_user(ctx, &handle, uhandle, sizeof(handle));
    if (ur != UACCESS_OK) return 0;
    return bind_socket(&handle, port, ip_version, ctx->id);
}

u64 syscall_socket_connect(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    uint8_t dst_kind = (uint8_t)ctx->PROC_X1;
    uintptr_t udst = (uintptr_t)ctx->PROC_X2;
    uint16_t port = (uint16_t)ctx->PROC_X3;

    SocketHandle handle = {};
    uaccess_result_t ur = copy_from_user(ctx, &handle, uhandle, sizeof(handle));
    if (ur != UACCESS_OK) return 0;

    net_l4_endpoint ep = {};
    char domain[256] = {};
    const void *dst = 0;

    if (dst_kind == DST_ENDPOINT) {
        ur = copy_from_user(ctx, &ep, udst, sizeof(ep));
        if (ur != UACCESS_OK) return 0;
        dst = &ep;
    } else if (dst_kind == DST_DOMAIN) {
        size_t copied = 0;
        bool term = false;
        ur = copy_str_from_user(ctx, domain, sizeof(domain), udst, &copied, &term);
        if (ur != UACCESS_OK) return 0;
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
    uaccess_result_t ur = copy_from_user(ctx, &handle, uhandle, sizeof(handle));
    if (ur != UACCESS_OK) return 0;
    return listen_on(&handle, backlog, ctx->id);
}

u64 syscall_socket_accept(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    SocketHandle handle = {};
    uaccess_result_t ur = copy_from_user(ctx, &handle, uhandle, sizeof(handle));
    if (ur != UACCESS_OK) return 0;
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
    uaccess_result_t ur = copy_from_user(ctx, &handle, uhandle, sizeof(handle));
    if (ur != UACCESS_OK) return 0;

    net_l4_endpoint ep = {};
    char domain[256] = {};
    const void *dst = 0;

    if (dst_kind == DST_ENDPOINT){
        ur = copy_from_user(ctx, &ep, udst, sizeof(ep));
        if (ur != UACCESS_OK) return 0;
        dst = &ep;
    } else if (dst_kind == DST_DOMAIN){
        size_t copied = 0;
        bool term = false;
        ur = copy_str_from_user(ctx, domain, sizeof(domain), udst, &copied, &term);
        if (ur != UACCESS_OK) return 0;
        if (!term) return 0;
        dst = domain;
    } else {
        return 0;
    }

    if (!size) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = zalloc(alloc_size);
    if (!kbuf) return 0;

    ur = copy_from_user(ctx, kbuf, ubuf, size);
    if (ur != UACCESS_OK){
        release(kbuf);
        return 0;
    }

    u64 r = send_on_socket(&handle, dst_kind, dst, port, kbuf, size, ctx->id);
    release(kbuf);
    return r;
}

u64 syscall_socket_receive(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    uintptr_t uout_src = (uintptr_t)ctx->PROC_X3;

    SocketHandle handle = {};
    uaccess_result_t ur = copy_from_user(ctx, &handle, uhandle, sizeof(handle));
    if (ur != UACCESS_OK) return 0;
    if (!size) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = zalloc(alloc_size);
    if (!kbuf) return 0;

    net_l4_endpoint src = {};
    i64 r = receive_from_socket(&handle, kbuf, size, &src, ctx->id);
    if (r > 0) {
        ur = copy_to_user(ctx, ubuf, kbuf, (size_t)r);
        if (ur != UACCESS_OK) r = ur;
    }
    if (r >= 0) {
        ur = copy_to_user(ctx, uout_src, &src, sizeof(src));
        if (ur != UACCESS_OK) r = ur;
    }

    release(kbuf);
    return r;
}

u64 syscall_socket_close(process_t *ctx){
    uintptr_t uhandle = (uintptr_t)ctx->PROC_X0;
    SocketHandle handle = {};
    uaccess_result_t ur = copy_from_user(ctx, &handle, uhandle, sizeof(handle));
    if (ur != UACCESS_OK) return 0;
    return close_socket(&handle, ctx->id);
}

u64 syscall_openf(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t udesc = (uintptr_t)ctx->PROC_X1;

    char req_path[255] = {};
    size_t copied = 0;
    bool term = false;
    uaccess_result_t ur = copy_str_from_user(ctx, req_path, sizeof(req_path), upath, &copied, &term);
    if (ur != UACCESS_OK) return 0;
    if (!term) return 0;

    char path[255] = {};
    if (!(ctx->PROC_PRIV) && req_path[0] != '/' && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s/%s", ctx->bundle, req_path);
    } else if (!(ctx->PROC_PRIV) && strstart_case("/resources/", req_path, true) == 11 && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s%s", ctx->bundle, req_path);
    } else {
        memcpy(path, req_path, strlen(req_path) + 1);
    }
    file descriptor = {};
    FS_RESULT r = open_file(path, &descriptor);
    if (r != FS_RESULT_SUCCESS) return r;
    ur = copy_to_user(ctx, udesc, &descriptor, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;
    return r;
}

u64 syscall_readf(process_t *ctx){
    uintptr_t udesc = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    file descriptor = {};
    uaccess_result_t ur = copy_from_user(ctx, &descriptor, udesc, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;

    uint8_t tmp[4096];
    size_t done = 0;

    while (done < size){
        size_t chunk = size - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);

        size_t r = read_file(&descriptor, (char*)tmp, chunk);
        if (!r) break;

        ur = copy_to_user(ctx, ubuf + (uintptr_t)done, tmp, r);
        if (ur != UACCESS_OK) {
            if (!done) done = ur;
            break;
        }

        done += r;
        if (r < chunk) break;
    }

    ur = copy_to_user(ctx, udesc, &descriptor, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;
    return done;
}

u64 syscall_writef(process_t *ctx){
    uintptr_t udesc = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;

    file descriptor = {};
    uaccess_result_t ur = copy_from_user(ctx, &descriptor, udesc, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;

    uint8_t tmp[4096];
    size_t done = 0;

    while (done < size) {
        size_t chunk = size - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);

        ur = copy_from_user(ctx, tmp, ubuf + (uintptr_t)done, chunk);
        if (ur != UACCESS_OK) {
            if (!done) done = ur;
            break;
        }

        size_t w = write_file(&descriptor, (const char*)tmp, chunk);
        if (!w) break;

        done += w;
        if (w < chunk) break;
    }

    ur = copy_to_user(ctx, udesc, &descriptor, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;
    return done;
}

u64 syscall_sreadf(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;

    char req_path[255] = {};
    size_t copied = 0;
    bool term = false;
    uaccess_result_t ur = copy_str_from_user(ctx, req_path, sizeof(req_path), upath, &copied, &term);
    if (ur != UACCESS_OK) return 0;
    if (!term) return 0;

    char path[255] = {};
    if (!(ctx->PROC_PRIV) && req_path[0] != '/' && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s/%s", ctx->bundle, req_path);
    } else if (!(ctx->PROC_PRIV) && strstart_case("/resources/", req_path, true) == 11 && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s%s", ctx->bundle, req_path);
    } else {
        memcpy(path, req_path, strlen(req_path) + 1);
    }

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = zalloc(alloc_size);
    if (!kbuf) return 0;

    size_t r = simple_read(path, kbuf, size);
    if (r) {
        ur = copy_to_user(ctx, ubuf, kbuf, r);
        if (ur != UACCESS_OK) {
            release(kbuf);
            return 0;
        }
    }

    release(kbuf);
    return r;
}

u64 syscall_swritef(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;

    char req_path[255] = {};
    size_t copied = 0;
    bool term = false;
    uaccess_result_t ur = copy_str_from_user(ctx, req_path, sizeof(req_path), upath, &copied, &term);
    if (ur != UACCESS_OK) return 0;
    if (!term) return 0;

    char path[255] = {};
    if (!(ctx->PROC_PRIV) && req_path[0] != '/' && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s/%s", ctx->bundle, req_path);
    } else if (!(ctx->PROC_PRIV) && strstart_case("/resources/", req_path, true) == 11 && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s%s", ctx->bundle, req_path);
    } else {
        memcpy(path, req_path, strlen(req_path) + 1);
    }

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = zalloc(alloc_size);
    if (!kbuf) return 0;

    ur = copy_from_user(ctx, kbuf, ubuf, size);
    if (ur != UACCESS_OK){
        release(kbuf);
        return 0;
    }

    size_t r = simple_write(path, kbuf, size);
    release(kbuf);
    return r;
}

u64 syscall_closef(process_t *ctx){
    uintptr_t udesc = (uintptr_t)ctx->PROC_X0;
    file descriptor = {};
    uaccess_result_t ur = copy_from_user(ctx, &descriptor, udesc, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;
    close_file(&descriptor);
    return 0;
}

u64 syscall_dir_list(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t ubuf = (uintptr_t)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    uintptr_t uoffset = (uintptr_t)ctx->PROC_X3;

    char req_path[255] = {};
    size_t copied = 0;
    bool term = false;
    uaccess_result_t ur = copy_str_from_user(ctx, req_path, sizeof(req_path), upath, &copied, &term);
    if (ur != UACCESS_OK) return 0;
    if (!term) return 0;

    char path[255] = {};
    if (!(ctx->PROC_PRIV) && req_path[0] != '/' && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s/%s", ctx->bundle, req_path);
    } else if (!(ctx->PROC_PRIV) && strstart_case("/resources/", req_path, true) == 11 && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s%s", ctx->bundle, req_path);
    } else {
        memcpy(path, req_path, strlen(req_path) + 1);
    }

    uint64_t off = 0;
    ur = copy_from_user(ctx, &off, uoffset, sizeof(off));
    if (ur != UACCESS_OK) return 0;

    uint64_t alloc_size = (size + 0xFFF) & ~0xFFFULL;
    void *kbuf = zalloc(alloc_size);
    if (!kbuf) return 0;

    size_t r = list_directory_contents(path, kbuf, size, &off);
    if (r) {
        ur = copy_to_user(ctx, ubuf, kbuf, r);
        if (ur != UACCESS_OK) {
            release(kbuf);
            return 0;
        }
    }
    ur = copy_to_user(ctx, uoffset, &off, sizeof(off));
    release(kbuf);
    if (ur != UACCESS_OK) return 0;
    return r;
}

u64 syscall_stat(process_t *ctx){
    uintptr_t upath = (uintptr_t)ctx->PROC_X0;
    uintptr_t uout = (uintptr_t)ctx->PROC_X1;

    char req_path[255] = {};
    size_t copied = 0;
    bool term = false;
    uaccess_result_t ur = copy_str_from_user(ctx, req_path, sizeof(req_path), upath, &copied, &term);
    if (ur != UACCESS_OK) return 0;
    if (!term) return 0;

    char path[255] = {};
    if (!(ctx->PROC_PRIV) && req_path[0] != '/' && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s/%s", ctx->bundle, req_path);
    } else if (!(ctx->PROC_PRIV) && strstart_case("/resources/", req_path, true) == 11 && ctx->bundle && *ctx->bundle) {
        string_format_buf(path, sizeof(path), "%s%s", ctx->bundle, req_path);
    } else {
        memcpy(path, req_path, strlen(req_path) + 1);
    }

    fs_stat st = {};
    FS_RESULT res = get_stat(path, &st);
    if (res != FS_RESULT_SUCCESS) return res;

    ur = copy_to_user(ctx, uout, &st, sizeof(st));
    if (ur != UACCESS_OK) return 0;
    return FS_RESULT_SUCCESS;
}

u64 syscall_trunc(process_t* ctx){
    uintptr_t udesc = (uintptr_t)ctx->PROC_X0;
    size_t size = (size_t)ctx->PROC_X1;
    file descriptor = {};
    uaccess_result_t ur = copy_from_user(ctx, &descriptor, udesc, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;
    bool ok = truncate(&descriptor, size);
    if (!ok) return 0;
    ur = copy_to_user(ctx, udesc, &descriptor, sizeof(descriptor));
    if (ur != UACCESS_OK) return 0;
    return ok;
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
    [KILL_PROCESS_CODE] = syscall_kill_process,
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
    [FILE_STAT_CODE] = syscall_stat,
    [FILE_TRNC_CODE] = syscall_trunc,
    // [LOAD_FSMODULE_CODE] = syscall_load_fsmod,
    // [UNLOAD_FSMODULE_CODE] = syscall_unload_fsmod,
    [IN_CASE_OF_JS_CODE] = syscall_in_case_of_js,
};

bool decode_crash_address_with_info(uint8_t depth, uintptr_t address, sizedptr debug_line, sizedptr debug_line_str){
    if (!debug_line.ptr || !debug_line.size) return false;
    debug_line_info info = dwarf_decode_lines(debug_line.ptr, debug_line.size, debug_line_str.ptr, debug_line_str.size, address);
    if (info.address == address){
        kprintf("[%.16x] %i: %s %i:%i", address, depth, info.file, info.line, info.column);
        return true;
    }
    return false;
}

bool decode_crash_address(uint8_t depth, uintptr_t address, sizedptr debug_line, sizedptr debug_line_str){
    return decode_crash_address_with_info(depth, address, debug_line, debug_line_str) ||
    decode_crash_address_with_info(depth, address, get_kernel_proc()->debug_lines, get_kernel_proc()->debug_line_str);
}

void backtrace(uintptr_t fp, uintptr_t elr, sizedptr debug_line, sizedptr debug_line_str) {

    if (elr){
        if (!decode_crash_address(0, elr, debug_line, debug_line_str))
            kprintf("Exception triggered by %llx",(elr));
    }

    for (uint8_t depth = 1; depth < 10 && fp; depth++) {
        int tr_ra = 0;
        uintptr_t ra_pa = mmu_translate(0, fp + 8, &tr_ra);
        if (tr_ra) return;

        uintptr_t return_address = (*(uintptr_t*)dmap_pa_to_kva((paddr_t)ra_pa));
        if (!return_address) return;
        return_address -= 4;//Return address is the next instruction after branching
        if (!decode_crash_address(depth, return_address, debug_line, debug_line_str))
            kprintf("%i: caller address: %llx", depth, return_address);
        int tr = 0;
        uintptr_t fp_pa = mmu_translate(0, fp, &tr);
        if (tr) return;
        fp = *(uintptr_t*)dmap_pa_to_kva((paddr_t)fp_pa);
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
    kprint(m);
    process_t *proc = get_current_proc();
    backtrace(sp, elr, proc->debug_lines, proc->debug_line_str);

    // for (int i = 0; i < 31; i++)
    //     kprintf("Reg[%i - %x] = %x",i,&proc->regs[i],proc->regs[i]);
    if (far > 0)
        debug_mmu_address(far);
    else
        kprintf("Null pointer accessed at %llx", elr);
}

void sync_el0_handler_c(){
    save_return_address_interrupt();
    mmu_ttbr0_disable_user();

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
            result = entry(proc);
        } else {
            kprintf("Unknown syscall in process. ESR: %llx. ELR: %llx. FAR: %llx", esr, elr, far);
            coredump(esr, elr, far, proc->sp);
            syscall_depth--;
            stop_current_process(ec);
        }
    } else {
        if (currentEL == 1){
                if (syscall_depth < 3){
                    if (syscall_depth < 1) kprintf("System has crashed. ESR: %llx. ELR: %llx. FAR: %llx", esr, elr, far);
                    if (syscall_depth < 2) {
                        uint64_t ksp = 0;
                        asm volatile ("mov %0, sp" : "=r"(ksp));
                        coredump(esr, elr, far, ksp);
                    }
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

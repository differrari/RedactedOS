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
#include "networking/interface_manager.h"
#include "bin/bin_mod.h"
#include "networking/transport_layer/csocket.h"
#include "loading/dwarf.h"
#include "sysregs.h"
#include "ui/graphic_types.h"


int syscall_depth = 0;
uintptr_t cpec;

//TODO: What happens if we pass another process' data in here?
typedef uint64_t (*syscall_entry)(process_t *ctx);

uint64_t syscall_malloc(process_t *ctx){
    void* page_ptr = (void*)mmu_translate(syscall_depth > 1 ? get_proc_by_pid(1)->heap : ctx->heap);
    if ((uintptr_t)page_ptr == 0x0){
        handle_exception("Wrong process heap state", 0);
    }
    size_t size = ctx->PROC_X0;
    return (uintptr_t)kalloc_inner(page_ptr, size, ALIGN_16B, get_current_privilege(), ctx->heap, &ctx->last_va_mapping, ctx->ttbr);
}

uint64_t syscall_free(process_t *ctx){
    kfree((void*)ctx->PROC_X0, ctx->PROC_X1);
    return 0;
}

uint64_t syscall_printl(process_t *ctx){
    kprint((const char *)ctx->PROC_X0);
    return 0;
}

uint64_t syscall_read_key(process_t *ctx){
    keypress *kp = (keypress*)ctx->PROC_X0;
    return sys_read_input_current(kp);
}

uint64_t syscall_read_event(process_t *ctx){
    kbd_event *ev = (kbd_event*)ctx->PROC_X0;
    return sys_read_event_current(ev);
}

uint64_t syscall_read_shortcut(process_t *ctx){
    kprint("[SYSCALL implementation error] Shortcut syscalls are not implemented yet");
    return 0;
}

uint64_t syscall_get_mouse(process_t *ctx){
    //TODO: do we want to prevent the process from knowing what the mouse is doing outside the window unless it's explicitly allowed?
    if (get_current_proc_pid() != ctx->id) return 0;
    mouse_data *inp = (mouse_data*)ctx->PROC_X0;
    inp->raw = get_raw_mouse_in();
    inp->position = convert_mouse_position(get_mouse_pos());
    return 0;
}

uintptr_t syscall_gpu_request_ctx(process_t *ctx){
    draw_ctx* d_ctx = (draw_ctx*)ctx->PROC_X0;
    get_window_ctx(d_ctx);
    return 0;
}

uint64_t syscall_gpu_flush(process_t *ctx){
    draw_ctx* d_ctx = (draw_ctx*)ctx->PROC_X0;
    commit_frame(d_ctx,0 );
    gpu_flush();
    return 0;
}

uint64_t syscall_gpu_resize_ctx(process_t *ctx){
    draw_ctx *d_ctx = (draw_ctx*)ctx->PROC_X0;
    uint32_t width = (uint32_t)ctx->PROC_X1;
    uint32_t height = (uint32_t)ctx->PROC_X2;
    resize_window(width, height);
    get_window_ctx(d_ctx);
    gpu_flush();
    return 0;
}

uint64_t syscall_char_size(process_t *ctx){
    return gpu_get_char_size(ctx->PROC_X0);
}

uint64_t syscall_msleep(process_t *ctx){
    syscall_depth--;
    sleep_process(ctx->PROC_X0);
    return 0;
}

uint64_t syscall_halt(process_t *ctx){
    kprintf("Process has ended with code %i",ctx->PROC_X0);
    syscall_depth--;
    stop_current_process(ctx->PROC_X0);
    return 0;
}

uint64_t syscall_exec(process_t *ctx){
    const char *prog_name = (const char*)ctx->PROC_X0;
    int argc = ctx->PROC_X1;
    const char **argv = (const char**)ctx->PROC_X2;
    process_t *p = execute(prog_name, argc, argv);
    if (p) p->win_id = ctx->win_id;
    return p ? p->id : 0;
}

uint64_t syscall_get_time(process_t *ctx){
    return timer_now_msec();
}

uint64_t syscall_socket_create(process_t *ctx){
    Socket_Role role = (Socket_Role)ctx->PROC_X0;
    protocol_t protocol = (protocol_t)ctx->PROC_X1;
    const SocketExtraOptions* extra = (const SocketExtraOptions*)ctx->PROC_X2;
    SocketHandle *out_handle = (SocketHandle*)ctx->PROC_X3;
    return create_socket(role, protocol, extra, ctx->id, out_handle);
}

uint64_t syscall_socket_bind(process_t *ctx){
    SocketHandle *handle = (SocketHandle*)ctx->PROC_X0;
    ip_version_t ip_version = (ip_version_t)ctx->PROC_X1;
    uint16_t port = (uint16_t)ctx->PROC_X2;
    return bind_socket(handle, port, ip_version, ctx->id);
}

uint64_t syscall_socket_connect(process_t *ctx){
    SocketHandle *handle = (SocketHandle*)ctx->PROC_X0;
    uint8_t dst_kind = (uint8_t)ctx->PROC_X1;
    void* dst = (void*)ctx->PROC_X2;
    uint16_t port = (uint16_t)ctx->PROC_X3;
    return connect_socket(handle, dst_kind, dst, port, ctx->id);
}

uint64_t syscall_socket_listen(process_t *ctx){
    SocketHandle *handle = (SocketHandle*)ctx->PROC_X0;
    int32_t backlog = (int32_t)ctx->PROC_X1;
    return listen_on(handle, backlog, ctx->id);
}

uint64_t syscall_socket_accept(process_t *ctx){
    SocketHandle *handle = (SocketHandle*)ctx->PROC_X0;
    accept_on_socket(handle, ctx->id);
    return 1;
}

uint64_t syscall_socket_send(process_t *ctx){
    SocketHandle *handle = (SocketHandle*)ctx->PROC_X0;
    uint8_t dst_kind = (uint8_t)ctx->PROC_X1;
    void* dst = (void*)ctx->PROC_X2;
    uint16_t port = (uint16_t)ctx->PROC_X3;
    void *ptr = (void*)ctx->PROC_X4;
    size_t size = (size_t)ctx->regs[5];
    return send_on_socket(handle, dst_kind, dst, port, ptr, size, ctx->id);
}

uint64_t syscall_socket_receive(process_t *ctx){
    SocketHandle *handle = (SocketHandle*)ctx->PROC_X0;
    void* buf = (void*)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    net_l4_endpoint* out_src = (net_l4_endpoint*)ctx->PROC_X3;
    return receive_from_socket(handle, buf, size, out_src, ctx->id);
}

uint64_t syscall_socket_close(process_t *ctx){
    SocketHandle *handle = (SocketHandle*)ctx->PROC_X0;
    return close_socket(handle, ctx->id);
}


uint64_t syscall_openf(process_t *ctx){
    char *req_path = (char *)ctx->PROC_X0;
    char path[255] = {};
    if (!(ctx->PROC_PRIV) && strstart_case("/resources/", req_path,true) == 11){
        string_format_buf(path, sizeof(path),"%s%s", ctx->bundle, req_path);
    } else memcpy(path, req_path, strlen(req_path) + 1);
    //TODO: Restrict access to own bundle, own fs and require privilege escalation for full-ish filesystem access
    file *descriptor = (file*)ctx->PROC_X1;
    return open_file(path, descriptor);
}

uint64_t syscall_readf(process_t *ctx){
    file *descriptor = (file*)ctx->PROC_X0;
    char *buf = (char*)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    return read_file(descriptor, buf, size);
}

uint64_t syscall_writef(process_t *ctx){
    file *descriptor = (file*)ctx->PROC_X0;
    char *buf = (char*)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    return write_file(descriptor, buf, size);
}

uint64_t syscall_sreadf(process_t *ctx){
    const char *path = (const char*)ctx->PROC_X0;
    void *buf = (void*)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    return simple_read(path, buf, size);
}

uint64_t syscall_swritef(process_t *ctx){
    const char *path = (const char*)ctx->PROC_X0;
    const void *buf = (void*)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    return simple_write(path, buf, size);
}

uint64_t syscall_closef(process_t *ctx){
    file *descriptor = (file*)ctx->PROC_X0;
    close_file(descriptor);
    return 0;
}

uint64_t syscall_dir_list(process_t *ctx){
    kprintf("[SYSCALL implementation error] directory listing not implemented yet");
    // char *path = (char *)ctx->PROC_X0;
    // return list_directory_contents(path);
    return 0;
}


syscall_entry syscalls[] = {
    [MALLOC_CODE] = syscall_malloc,
    [FREE_CODE] = syscall_free,
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
        uintptr_t return_address = (*(uintptr_t*)(fp + 8));

        if (return_address != 0){
            return_address -= 4;//Return address is the next instruction after branching
            if (!decode_crash_address(depth, return_address, debug_line, debug_line_str))
                kprintf("%i: caller address: %llx", depth, return_address, return_address);
            fp = *(uintptr_t*)fp;
            if (!mmu_translate(fp)) return;
        } else return;

    }
}

const char* fault_messages[] = {
    [0b000000] = "Address size fault in TTBR0 or TTBR1",
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

    kprint(fault_messages[ifsc]);
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

    uint64_t result = 0;
    if (ec == 0x15) {
        if (syscalls[iss]){
            result = syscalls[iss](proc);
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
                kprintf("Process has crashed. ESR: %llx. ELR: %llx. FAR: %llx", esr, elr, far);
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

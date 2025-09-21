#include "syscall.h"
#include "console/kio.h"
#include "exceptions/exception_handler.h"
#include "console/serial/uart.h"
#include "exceptions/irq.h"
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

int syscall_depth = 0;

//TODO: What happens if we pass another process' data in here?
//TODO: make indexmap in c and it can be used here
typedef struct {
    uint16_t syscall_num;
    uint64_t (*syscall)(process_t *ctx);
} syscall_entry;

uint64_t syscall_malloc(process_t *ctx){
    void* page_ptr = syscall_depth > 1 ? (void*)get_proc_by_pid(1)->heap : (void*)get_current_heap();
    if ((uintptr_t)page_ptr == 0x0){
        handle_exception("Wrong process heap state", 0);
    }
    return (uintptr_t)kalloc(page_ptr, ctx->PROC_X0, ALIGN_16B, get_current_privilege());
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

uint64_t syscall_read_shortcut(process_t *ctx){
    kprint("[SYSCALL implementation error] Shortcut syscalls are not implemented yet");
    return 0;
}

uint64_t syscall_get_mouse(process_t *ctx){
    //TODO: do we want to prevent the process from knowing what the mouse is doing outside the window unless it's explicitly allowed?
    if (get_current_proc_pid() != ctx->id) return 0;
    mouse_input *inp = (mouse_input*)ctx->PROC_X0;
    *inp = get_raw_mouse_in();
    return 0;
}

uintptr_t syscall_gpu_request_ctx(process_t *ctx){
    draw_ctx* d_ctx = (draw_ctx*)ctx->PROC_X0;
    get_window_ctx(d_ctx);
    return 0;
}

uint64_t syscall_gpu_flush(process_t *ctx){
    draw_ctx* d_ctx = (draw_ctx*)ctx->PROC_X0;
    commit_frame(d_ctx);
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
    return gpu_get_char_size(ctx->PROC_X0);;
}

uint64_t syscall_sleep(process_t *ctx){
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

uint64_t syscall_get_time(process_t *ctx){
    return timer_now_msec();
}

uint64_t syscall_bind_port(process_t *ctx){
    kprintf("[SYSCALL implementation error] syscall %s not implemented",__func__);
    // uint16_t port     = (uint16_t)ctx->PROC_X0;
    // port_recv_handler_t handler = (port_recv_handler_t)ctx->PROC_X1;
    // protocol_t proto  = (protocol_t)ctx->PROC_X2;
    // uint16_t pid      = get_current_proc_pid();
    // return port_bind_manual(port, pid, proto, handler);
    return 0;
}

uint64_t syscall_unbind_port(process_t *ctx){
    kprintf("[SYSCALL implementation error] syscall %s not implemented",__func__);
    // uint16_t port    = (uint16_t)ctx->PROC_X0;
    // protocol_t proto = (protocol_t)ctx->PROC_X2;
    // uint16_t pid     = get_current_proc_pid();
    // return port_unbind(port, proto, pid);
    return 0;
}

uint64_t syscall_send_packet(process_t *ctx){
    kprintf("[SYSCALL implementation error] syscall %s not implemented",__func__);
    // uintptr_t frame_ptr = ctx->PROC_X0;
    // uint32_t  frame_len = (uint32_t)ctx->PROC_X1;
    // return net_tx_frame(frame_ptr, frame_len);
    return 0;
}

uint64_t syscall_read_packet(process_t *ctx){
    kprintf("[SYSCALL implementation error] syscall %s not implemented",__func__);
    // sizedptr *user_out = (sizedptr*)ctx->PROC_X0;
    // return net_rx_frame(user_out);
    return 0;
}

uint64_t syscall_fopen(process_t *ctx){
    char *req_path = (char *)ctx->PROC_X0;
    char path[255];
    if (!(ctx->spsr & 0x200)){
        // path = 
        if (strstart("/resources/", req_path, true)){
            string_format_buf("%s%s", path, ctx->bundle, req_path);
        } else return 0;//In the future, we'll allow a documents path as well as privilege escalation for full-ish filesystem access
    } else memcpy(path, req_path, strlen(req_path, 0));
    kprint(path);
    file *descriptor = (file*)ctx->PROC_X1;
    return open_file(path, descriptor);
}

uint64_t syscall_fread(process_t *ctx){
    file *descriptor = (file*)ctx->PROC_X0;
    char *buf = (char*)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    return read_file(descriptor, buf, size);
}

uint64_t syscall_fwrite(process_t *ctx){
    file *descriptor = (file*)ctx->PROC_X0;
    char *buf = (char*)ctx->PROC_X1;
    size_t size = (size_t)ctx->PROC_X2;
    return write_file(descriptor, buf, size);
}

uint64_t syscall_dir_list(process_t *ctx){
    kprintf("[SYSCALL implementation error] directory listing not implemented yet");
    // char *path = (char *)ctx->PROC_X0;
    // return list_directory_contents(path);
    return 0;
}

syscall_entry syscalls[] = {
    { MALLOC_CODE, syscall_malloc},
    { FREE_CODE, syscall_free},
    { PRINTL_CODE, syscall_printl},
    { READ_KEY_CODE, syscall_read_key},
    { READ_SHORTCUT_CODE, syscall_read_shortcut},
    { GET_MOUSE_STATUS_CODE, syscall_get_mouse },
    { REQUEST_DRAW_CTX_CODE, syscall_gpu_request_ctx},
    { GPU_FLUSH_DATA_CODE, syscall_gpu_flush},
    { GPU_CHAR_SIZE_CODE, syscall_char_size},
    { RESIZE_DRAW_CTX_CODE, syscall_gpu_resize_ctx},
    { SLEEP_CODE, syscall_sleep},
    { HALT_CODE, syscall_halt},
    { GET_TIME_CODE, syscall_get_time},
    { BIND_PORT_CODE, syscall_bind_port},
    { UNBIND_PORT_CODE, syscall_unbind_port},
    { SEND_PACKET_CODE, syscall_send_packet},
    { READ_PACKET_CODE, syscall_read_packet},
    {FILE_OPEN_CODE, syscall_fopen},
    {FILE_READ_CODE, syscall_fread},
    {FILE_WRITE_CODE, syscall_fwrite},
    {DIR_LIST_CODE, syscall_dir_list},
};

void coredump(uint64_t esr, uint64_t elr, uint64_t far){
    // uint8_t ifsc = esr & 0x3F;
    // 0b000000	Address size fault in TTBR0 or TTBR1.

    // 0b000101	Translation fault, 1st level.
    // 00b00110	Translation fault, 2nd level.
    // 00b00111	Translation fault, 3rd level.

    // 0b001001	Access flag fault, 1st level.
    // 0b001010	Access flag fault, 2nd level.
    // 0b001011	Access flag fault, 3rd level.

    // 0b001101	Permission fault, 1st level.
    // 0b001110	Permission fault, 2nd level.
    // 0b001111	Permission fault, 3rd level.

    // 0b010000	Synchronous external abort.
    // 0b011000	Synchronous parity error on memory access.
    // 0b010101	Synchronous external abort on translation table walk, 1st level.
    // 0b010110	Synchronous external abort on translation table walk, 2nd level.
    // 0b010111	Synchronous external abort on translation table walk, 3rd level.
    // 0b011101	Synchronous parity error on memory access on translation table walk, 1st level.
    // 0b011110	Synchronous parity error on memory access on translation table walk, 2nd level.
    // 0b011111	Synchronous parity error on memory access on translation table walk, 3rd level.
    
    // 0b100001	Alignment fault.
    // 0b100010	Debug event.
    //TODO: Can parse instruction class, fault cause, etc
    decode_instruction(*(uint32_t*)elr);
    // process_t *proc = get_current_proc();
    // for (int i = 0; i < 31; i++)
    //     kprintf("Reg[%i - %x] = %x",i,&proc->regs[i],proc->regs[i]);
    if (far > 0) 
        debug_mmu_address(far);
    else 
        kprintf("Null pointer accessed at %x",elr);
}

void sync_el0_handler_c(){
    save_context_registers();
    save_return_address_interrupt();

    syscall_depth++;

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
    
    uint64_t result = 0;
    if (ec == 0x15) {
        uint16_t num_syscalls = N_ARR(syscalls);
        bool found = false;
        for (uint16_t i = 0; i < num_syscalls; i++){
            if (syscalls[i].syscall_num == iss){
                found = true;
                result = syscalls[i].syscall(proc);
                break;
            }
        }
        if (!found)
            panic("Unknown syscall %i", iss);
    } else {
        uint64_t far;
        asm volatile ("mrs %0, far_el1" : "=r"(far));
        if (far == 0 && elr == 0 && currentEL == 0){
            kprintf("Process has exited %x",x0);
            syscall_depth--;
            stop_current_process(x0);
        }// else kprintf("ELR %x FAR %x",elr,far);
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
        //We could handle more exceptions now, such as x25 (unmasked x96) = data abort. 0x21 at end of 0x25 = alignment fault
        if (currentEL == 1){
            kprintf("System has crashed. ESR: %x. ELR: %x. FAR: %x", esr, elr, far);
            coredump(esr, elr, far);
            handle_exception("UNEXPECTED EXCEPTION",ec);
        } else {
            kprintf("Process has crashed. ESR: %x. ELR: %x. FAR: %x", esr, elr, far);
            coredump(esr, elr, far);
            syscall_depth--;
            stop_current_process(ec);
        }
    }
    syscall_depth--;
    save_syscall_return(result);
    process_restore();
}
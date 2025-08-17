#include "syscall.h"
#include "console/kio.h"
#include "exceptions/exception_handler.h"
#include "console/serial/uart.h"
#include "exceptions/irq.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "graph/graphics.h"
#include "memory/memory_access.h"
#include "input/input_dispatch.h"
#include "std/memfunctions.h"
#include "std/string.h"
#include "exceptions/timer.h"
#include "networking/network.h"
#include "networking/port_manager.h"
#include "filesystem/filesystem.h"
#include "syscalls/syscall_codes.h"

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
        handle_exception("Wrong process heap state");
    }
    return (uintptr_t)kalloc(page_ptr, ctx->PROC_X0, ALIGN_16B, get_current_privilege(), false);
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

uint64_t syscall_clear_screen(process_t *ctx){
    gpu_clear(ctx->PROC_X0 & UINT32_MAX);
    return 0;
}

uint64_t syscall_draw_pixel(process_t *ctx){
    gpu_draw_pixel(*(gpu_point*)ctx->PROC_X0,ctx->PROC_X1);
    return 0;
}

uint64_t syscall_draw_line(process_t *ctx){
    gpu_draw_line(*(gpu_point*)ctx->PROC_X0,*(gpu_point*)ctx->PROC_X1, ctx->PROC_X2 & UINT32_MAX);
    return 0;
}

uint64_t syscall_draw_rect(process_t *ctx){
    gpu_fill_rect(*(gpu_rect*)ctx->PROC_X0,ctx->PROC_X1 & UINT32_MAX);
    return 0;
}

uint64_t syscall_draw_char(process_t *ctx){
    gpu_draw_char(*(gpu_point*)ctx->PROC_X0,(char)(ctx->PROC_X1 & 0xFF),ctx->PROC_X2,ctx->PROC_X3 & UINT32_MAX);
    return 0;
}

uint64_t syscall_draw_string(process_t *ctx){
    gpu_draw_string(*(string *)ctx->PROC_X0,*(gpu_point*)ctx->PROC_X1,ctx->PROC_X2,ctx->PROC_X3 & UINT32_MAX);
    return 0;
}

uint64_t syscall_gpu_request_ctx(process_t *ctx){
    draw_ctx *dctx = (draw_ctx*)ctx->PROC_X0;
    gpu_get_ctx(dctx);
    return 0;
}

uint64_t syscall_gpu_flush(process_t *ctx){
    gpu_flush();
    return 0;
}

//TODO: do not allocate memory for the process, let it provide its own
uint64_t syscall_screen_size(process_t *ctx){
    uint64_t result = (uintptr_t)kalloc((void*)get_current_heap(), sizeof(gpu_size), ALIGN_16B, get_current_privilege(), false);
    gpu_size size = gpu_get_screen_size();
    memcpy((void*)result, &size, sizeof(gpu_size));
    return result;
}

uint64_t syscall_char_size(process_t *ctx){
    return gpu_get_char_size(ctx->PROC_X0);;
}

uint64_t syscall_sleep(process_t *ctx){
    sleep_process(ctx->PROC_X0);
    return 0;
}

uint64_t syscall_halt(process_t *ctx){
    stop_current_process(ctx->PROC_X0);
    return 0;
}

uint64_t syscall_get_time(process_t *ctx){
    return timer_now_msec();
}

uint64_t syscall_bind_port(process_t *ctx){
    uint16_t port     = (uint16_t)ctx->PROC_X0;
    port_recv_handler_t handler = (port_recv_handler_t)ctx->PROC_X1;
    protocol_t proto  = (protocol_t)ctx->PROC_X2;
    uint16_t pid      = get_current_proc_pid();
    return port_bind_manual(port, pid, proto, handler);
}

uint64_t syscall_unbind_port(process_t *ctx){
    uint16_t port    = (uint16_t)ctx->PROC_X0;
    protocol_t proto = (protocol_t)ctx->PROC_X2;
    uint16_t pid     = get_current_proc_pid();
    return port_unbind(port, proto, pid);
}

uint64_t syscall_send_packet(process_t *ctx){
    uintptr_t frame_ptr = ctx->PROC_X0;
    uint32_t  frame_len = (uint32_t)ctx->PROC_X1;
    return net_tx_frame(frame_ptr, frame_len);
}

uint64_t syscall_read_packet(process_t *ctx){
    sizedptr *user_out = (sizedptr*)ctx->PROC_X0;
    return net_rx_frame(user_out);
}

syscall_entry syscalls[] = {
    { MALLOC_CODE, syscall_malloc},
    { FREE_CODE, syscall_free},
    { PRINTL_CODE, syscall_printl},
    { READ_KEY_CODE, syscall_read_key},
    { READ_SHORTCUT_CODE, syscall_read_shortcut},
    { CLEAR_SCREEN_CODE, syscall_clear_screen},
    { DRAW_PRIMITIVE_PIXEL_CODE, syscall_draw_pixel},
    { DRAW_PRIMITIVE_LINE_CODE, syscall_draw_line},
    { DRAW_PRIMITIVE_RECT_CODE, syscall_draw_rect},
    { DRAW_PRIMITIVE_CHAR_CODE, syscall_draw_char},
    { DRAW_PRIMITIVE_STRING_CODE, syscall_draw_string},
    { REQUEST_DRAW_CTX_CODE, syscall_gpu_request_ctx},
    { GPU_FLUSH_DATA_CODE, syscall_gpu_flush},
    { GPU_SCREEN_SIZE_CODE, syscall_screen_size},
    { GPU_CHAR_SIZE_CODE, syscall_char_size},
    { SLEEP_CODE, syscall_sleep},
    { HALT_CODE, syscall_halt},
    { GET_TIME_CODE, syscall_get_time},
    { BIND_PORT_CODE, syscall_bind_port},
    { UNBIND_PORT_CODE, syscall_unbind_port},
    { SEND_PACKET_CODE, syscall_send_packet},
    { READ_PACKET_CODE, syscall_read_packet},
};

void sync_el0_handler_c(){
    save_context_registers();
    save_return_address_interrupt();

    syscall_depth++;
    
    if (ksp > 0)
        asm volatile ("mov sp, %0" :: "r"(ksp));

    process_t *proc = get_current_proc();

    uint64_t x0 = proc->PROC_X0;
    uint64_t elr;
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    uint64_t spsr;
    asm volatile ("mrs %0, spsr_el1" : "=r"(spsr));

    uint64_t currentEL = (spsr >> 2) & 3;

    uint64_t sp_el;
    asm volatile ("mov %0, x11" : "=r"(sp_el));
    
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
        if (!found){
            kprintf("Unknown syscall %i", iss);
        }
    } else {
        switch (ec) {
            case 0x21: {
                uint64_t far;
                asm volatile ("mrs %0, far_el1" : "=r"(far));
                if (far == 0){
                    kprintf("Process has exited %x",x0);
                    stop_current_process(x0);
                }
            }
        }
        //We could handle more exceptions now, such as x25 (unmasked x96) = data abort. 0x21 at end of 0x25 = alignment fault
        if (currentEL == 1)
            handle_exception_with_info("UNEXPECTED EXCEPTION",ec);
        else {
            uint64_t far;
            asm volatile ("mrs %0, far_el1" : "=r"(far));
            kprintf("Process has crashed. ESR: %x. ELR: %x. FAR: %x", esr, elr, far);
            stop_current_process(ec);
        }
    }
    syscall_depth--;
    if (result > 0)
        save_syscall_return(result);
    process_restore();
}
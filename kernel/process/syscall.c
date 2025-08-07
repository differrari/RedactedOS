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
#include "kernel_processes/windows/windows.h"
#include "std/memfunctions.h"
#include "std/string.h"
#include "exceptions/timer.h"
#include "networking/network.h"
#include "networking/port_manager.h"

void sync_el0_handler_c(){
    save_context_registers();
    save_return_address_interrupt();
    
    if (ksp > 0)
        asm volatile ("mov sp, %0" :: "r"(ksp));
    uint64_t x0;
    asm volatile ("mov %0, x15" : "=r"(x0));
    uint64_t x1;
    asm volatile ("mov %0, x14" : "=r"(x1));
    uint64_t x2;
    asm volatile ("mov %0, x9" : "=r"(x2));
    uint64_t x3;
    asm volatile ("mov %0, x16" : "=r"(x3));
    uint64_t x4;
    asm volatile ("mov %0, x4" : "=r"(x4));
    uint64_t x29;
    asm volatile ("mov %0, x13" : "=r"(x29));
    uint64_t x30;
    asm volatile ("mov %0, x12" : "=r"(x30));
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
        switch (iss)
        {
        case 0:
            void* page_ptr = (void*)get_current_heap();
            if ((uintptr_t)page_ptr == 0x0){
                handle_exception_with_info("Wrong process heap state", iss);
            }
            result = (uintptr_t)kalloc(page_ptr, x0, ALIGN_16B, get_current_privilege(), false);
            break;
        case 1:
            kfree((void*)x0, x1);
            break;
        case 3:
            kprint((const char *)x0);
            break;

        case 5:
            keypress *kp = (keypress*)x0;
            result = sys_read_input_current(kp);
            break;

        case 10:
            if (!screen_overlay)
                gpu_clear(x0);
            break;

        case 11:
            if (!screen_overlay)
                gpu_draw_pixel(*(gpu_point*)x0,x1);
            break;

        case 12:
            if (!screen_overlay)
                gpu_draw_line(*(gpu_point*)x0,*(gpu_point*)x1,x2);
            break;

        case 13:
            if (!screen_overlay)
                gpu_fill_rect(*(gpu_rect*)x0,x1);
            break;

        case 14:
            if (!screen_overlay)
                gpu_draw_char(*(gpu_point*)x0,(char)x1,x2,x3);
            break;

        case 15:
            if (!screen_overlay){
                gpu_draw_string(*(string *)x0,*(gpu_point*)x1,x2,x3);
            }
            break;

        case 20:
            if (!screen_overlay)
                gpu_flush();
            break;

        case 21:
            result = (uintptr_t)kalloc((void*)get_current_heap(), sizeof(gpu_size), ALIGN_16B, get_current_privilege(), false);
            gpu_size size = gpu_get_screen_size();
            memcpy((void*)result, &size, sizeof(gpu_size));
            break;

        case 22:
            result = gpu_get_char_size(x0);
            break;

        case 30:
            sleep_process(x0);
            break;
        
        case 33:
            stop_current_process();
            break;

        case 40:
            result = timer_now_msec();
            break;

        case 51: {  //bind
            uint16_t port     = (uint16_t)x0;
            port_recv_handler_t handler = (port_recv_handler_t)x1;
            protocol_t proto  = (protocol_t)x2;
            uint16_t pid      = get_current_proc_pid();
            result = port_bind_manual(port, pid, proto, handler);
            break;
        }

        case 52: {  //unbind
            uint16_t port    = (uint16_t)x0;
            protocol_t proto = (protocol_t)x2;
            uint16_t pid     = get_current_proc_pid();
            result = port_unbind(port, proto, pid);
            break;
        }

        case 53: { //net_tx_frame
            uintptr_t frame_ptr = x0;
            uint32_t  frame_len = (uint32_t)x1;
            result = net_tx_frame(frame_ptr, frame_len);
            break;
        }
        case 54: { //net_rx_frame
            sizedptr *user_out = (sizedptr*)x0;
            result = net_rx_frame(user_out);
            break;
        }
        
        default:
            handle_exception_with_info("Unknown syscall", iss);
            break;
        }
    } else {
        //We could handle more exceptions now, such as x25 (unmasked x96) = data abort
        if (currentEL == 1)
            handle_exception_with_info("UNEXPECTED EXCEPTION",ec);
        else {
            uint64_t far;
            asm volatile ("mrs %0, far_el1" : "=r"(far));
            kprintf("Process has crashed. ESR: %x. ELR: %x. FAR: %x", esr, elr, far);
            stop_current_process();
        }
    }
    if (result > 0)
        save_syscall_return(result);
    process_restore();
}


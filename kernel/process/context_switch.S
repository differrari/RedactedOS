.global save_context
save_context:
    // x0: pointer to process_t

    // Save general-purpose registers x1-x30 (excluding x0 which holds the pointer)
    stp x1, x2, [x0, #(8 * 0)]
    stp x3, x4, [x0, #(8 * 2)]
    stp x5, x6, [x0, #(8 * 4)]
    stp x7, x8, [x0, #(8 * 6)]
    stp x9, x10, [x0, #(8 * 8)]
    stp x11, x12, [x0, #(8 * 10)]
    stp x13, x14, [x0, #(8 * 12)]
    stp x15, x16, [x0, #(8 * 14)]
    stp x17, x18, [x0, #(8 * 16)]
    stp x19, x20, [x0, #(8 * 18)]
    stp x21, x22, [x0, #(8 * 20)]
    stp x23, x24, [x0, #(8 * 22)]
    stp x25, x26, [x0, #(8 * 24)]
    stp x27, x28, [x0, #(8 * 26)]
    stp x29, x30, [x0, #(8 * 28)]

    // Save SP
    str x11, [x0, #(8 * 31)]

    //Save status bits
    str x10, [x0, #(8 * 33)]

    ret

.global save_pc_interrupt
save_pc_interrupt:
    // x0: pointer to process_t
    
    mrs x1, elr_el1
    str x1, [x0, #(8 * 32)]//PC

    ret

.global restore_context
restore_context:
    // x0: pointer to process_t

    // Restore general-purpose registers x1-x30
    ldp x1, x2, [x0, #(8 * 0)]
    ldp x3, x4, [x0, #(8 * 2)]
    ldp x5, x6, [x0, #(8 * 4)]
    ldp x7, x8, [x0, #(8 * 6)]
    ldp x9, x10, [x0, #(8 * 8)]
    ldp x11, x12, [x0, #(8 * 10)]
    ldp x13, x14, [x0, #(8 * 12)]
    ldp x15, x16, [x0, #(8 * 14)]
    ldp x17, x18, [x0, #(8 * 16)]
    ldp x19, x20, [x0, #(8 * 18)]
    ldp x21, x22, [x0, #(8 * 20)]
    ldp x23, x24, [x0, #(8 * 22)]
    ldp x25, x26, [x0, #(8 * 24)]
    ldp x27, x28, [x0, #(8 * 26)]
    ldp x29, x30, [x0, #(8 * 28)]    

    ldr x11, [x0, #(8 * 31)]
    mov sp, x11
    msr sp_el0, x11
    mov x29, x11
    ldr x11, [x0, #(8 * 33)]
    msr spsr_el1, x11
    ldr x11, [x0, #(8 * 32)]
    msr elr_el1, x11
    
    mov x0, x15
    mov x1, x14
    mov x30, x12

    eret

.global restore_context_yield
restore_context_yield:
    // x0: pointer to process_t

    // Restore general-purpose registers x1-x30
    ldp x1, x2, [x0, #(8 * 0)]
    ldp x3, x4, [x0, #(8 * 2)]
    ldp x5, x6, [x0, #(8 * 4)]
    ldp x7, x8, [x0, #(8 * 6)]
    ldp x9, x10, [x0, #(8 * 8)]
    ldp x11, x12, [x0, #(8 * 10)]
    ldp x13, x14, [x0, #(8 * 12)]
    ldp x15, x16, [x0, #(8 * 14)]
    ldp x17, x18, [x0, #(8 * 16)]
    ldp x19, x20, [x0, #(8 * 18)]
    ldp x21, x22, [x0, #(8 * 20)]
    ldp x23, x24, [x0, #(8 * 22)]
    ldp x25, x26, [x0, #(8 * 24)]
    ldp x27, x28, [x0, #(8 * 26)]
    ldp x29, x30, [x0, #(8 * 28)]    

    ldr x11, [x0, #(8 * 31)]
    mov sp, x11
    mov x29, x11
    msr sp_el0, x11
    #ldr x11, [x0, #(8 * 33)]
    mov x11, #0xC0
    msr spsr_el1, x11
    ldr x11, [x0, #(8 * 32)]
    mov x0, x15
    mov x1, x14
    mov x30, x12

    br x11

.global restore_pc_interrupt
restore_pc_interrupt:
    ldr x1, [x0, #(8 * 32)]
    msr elr_el1, x1
    eret;
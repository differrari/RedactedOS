.global job_save_kernel
job_save_kernel:

    stp x17, x18, [sp, #-16]!
    add x17, sp, #16
2:
    adrp x18, job_kpec
    add x18, x18, :lo12:job_kpec
    ldr x18, [x18]

3:
    
// Save general-purpose registers x1-x30
stp x0, x1, [x18, #(8 * 0)]
stp x2, x3, [x18, #(8 * 2)]
stp x4, x5, [x18, #(8 * 4)]
stp x6, x7, [x18, #(8 * 6)]
stp x8, x9, [x18, #(8 * 8)]
stp x10, x11, [x18, #(8 * 10)]
stp x12, x13, [x18, #(8 * 12)]
stp x14, x15, [x18, #(8 * 14)]
str x16,      [x18, #(8 * 16)]
ldr x16, [sp, #0]
str x16, [x18, #(8 * 17)]
ldr x16, [sp, #8]
str x16, [x18, #(8 * 18)]
str      x19, [x18, #(8 * 19)]
stp x20, x21, [x18, #(8 * 20)]
stp x22, x23, [x18, #(8 * 22)]
stp x24, x25, [x18, #(8 * 24)]
stp x26, x27, [x18, #(8 * 26)]
stp x28, x29, [x18, #(8 * 28)]
str x30,      [x18,#(8 * 30)] 

// SP
str x17, [x18, #(8 * 31)]

//Status bits
mrs x17, spsr_el1
str x17, [x18, #(8 * 33)]

add sp, sp, #16

ret

.global save_kstack
save_kstack:

sub sp, sp, #32
stp x0, x1, [sp, #0]
stp x21,x30, [sp, #16]

adrp x0, ksp
add  x0, x0, :lo12:ksp

mov x1, sp
sub x21, x0, x1
mov x0, #0x10000
bl zalloc

adrp x1, job_kstack
add x1, x1, :lo12:job_kstack
stp x0, x21, [x1, #0]

add x0,x0,#0x10000
sub x0,x0,x21

mov x1, sp
mov x2, x21
bl memcpy

ldp x0, x1, [sp, #0]
ldp x21,x30, [sp, #16]

add sp, sp, #32

ret

.global job_restore_kernel
job_restore_kernel:
    adrp x18, job_kpec
    add x18, x18, :lo12:job_kpec
    ldr x18, [x18]
    // Restore general-purpose registers
    ldp x0, x1, [x18, #(8 * 0)]
    ldp x2, x3, [x18, #(8 * 2)]
    ldp x4, x5, [x18, #(8 * 4)]
    ldp x6, x7, [x18, #(8 * 6)]
    ldp x8, x9, [x18, #(8 * 8)]
    ldp x10, x11, [x18, #(8 * 10)]
    ldp x12, x13, [x18, #(8 * 12)]
    ldp x14, x15, [x18, #(8 * 14)]
    ldr x16,      [x18, #(8 * 16)]
    ldr      x19, [x18, #(8 * 19)]
    ldp x20, x21, [x18, #(8 * 20)]
    ldp x22, x23, [x18, #(8 * 22)]
    ldp x24, x25, [x18, #(8 * 24)]
    ldp x26, x27, [x18, #(8 * 26)]
    ldp x28, x29, [x18, #(8 * 28)]
    ldr x30,      [x18, #(8 * 30)]    

    ldr x17, [x18, #(8 * 33)]
    msr spsr_el1, x17
    lsr     x17, x17, #2
    and     x17, x17, #0b11

    ldr x17, [x18, #(8 * 31)]
    mov     sp, x17

    ldr x17, [x18, #(8 * 17)]
    ldr x30, [x18, #(8 * 32)]
    ldr x18, [x18, #(8 * 18)]

    dsb ishst
    tlbi vmalle1is
    dsb ish
    isb

    ret

.global job_save_ret
job_save_ret:
    ldr x0, [sp, #8]
    
    ret


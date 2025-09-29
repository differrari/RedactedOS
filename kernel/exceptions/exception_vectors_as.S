#include "process/context_save.S"

.section .vectors, "a", %progbits
.align 11
.global exception_vectors

exception_vectors:

.macro vector_slot handler
    b \handler
    .space 0x80 - 4
.endm

    vector_slot sync_el0_handler_as      // EL1t sync
    vector_slot irq_el1_asm_handler       // EL1t irq
    vector_slot fiq_el1_handler       // EL1t fiq
    vector_slot error_el1_handler     // EL1t serror

    vector_slot sync_el0_handler_as      // EL1h sync
    vector_slot irq_el1_asm_handler       // EL1h irq
    vector_slot fiq_el1_handler       // EL1h fiq
    vector_slot error_el1_handler     // EL1h serror

    vector_slot sync_el0_handler_as      // EL0_64 sync
    vector_slot irq_el1_asm_handler       // EL0_64 irq
    vector_slot fiq_el1_handler       // EL0_64 fiq
    vector_slot error_el1_handler     // EL0_64 serror

    vector_slot sync_el0_handler_as      // EL0_32 sync
    vector_slot irq_el1_asm_handler       // EL0_32 irq
    vector_slot fiq_el1_handler       // EL0_32 fiq
    vector_slot error_el1_handler     // EL0_32 serror

.global irq_el1_asm_handler
irq_el1_asm_handler:
    save_ctx 

    b irq_el1_handler
    eret

.global sync_el0_handler_as
//TODO: Rethink the registers used to be sequential both here and in context_switch and exception_vectors_as
sync_el0_handler_as:
    save_ctx

    b sync_el0_handler_c
    eret
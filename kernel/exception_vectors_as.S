.section .vectors, "a", %progbits
.align 11
.global exception_vectors

exception_vectors:

.macro vector_slot handler
    b \handler
    .space 0x80 - 4
.endm

    vector_slot sync_el1_handler      // EL1t sync
    vector_slot irq_el1_handler       // EL1t irq
    vector_slot fiq_el1_handler       // EL1t fiq
    vector_slot error_el1_handler     // EL1t serror

    vector_slot sync_el1_handler      // EL1h sync
    vector_slot irq_el1_handler       // EL1h irq
    vector_slot fiq_el1_handler       // EL1h fiq
    vector_slot error_el1_handler     // EL1h serror

    vector_slot sync_el1_handler      // EL0_64 sync
    vector_slot irq_el1_handler       // EL0_64 irq
    vector_slot fiq_el1_handler       // EL0_64 fiq
    vector_slot error_el1_handler     // EL0_64 serror

    vector_slot sync_el1_handler      // EL0_32 sync
    vector_slot irq_el1_handler       // EL0_32 irq
    vector_slot fiq_el1_handler       // EL0_32 fiq
    vector_slot error_el1_handler     // EL0_32 serror
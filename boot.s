.global _start
_start:
    ldr sp, =stack_top        // Set up stack
    bl kernel_main            
1:  b .
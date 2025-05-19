.macro syscall_def name:req, id:req
.global \name
\name:
    svc #\id
    ret
.endm

syscall_def malloc, 0
syscall_def free, 1

syscall_def printf_args, 3

syscall_def read_key, 4
syscall_def register_shortcut, 5
syscall_def read_shortcut, 6

//Primitives
syscall_def draw_primitive_pixel, 10
syscall_def draw_primitive_line, 11
syscall_def draw_primitive_rect, 12
syscall_def draw_primitive_char, 13
syscall_def draw_primitive_text, 14

//GPU commands
syscall_def gpu_flush_data, 20

syscall_def process_sleep, 30
syscall_def process_yield, 31
// syscall_def process_await, 32
syscall_def process_terminate, 33

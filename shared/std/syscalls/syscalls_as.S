.macro syscall_def name:req, id:req
.global \name
.type \name, %function
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
syscall_def clear_screen, 10
syscall_def draw_primitive_pixel, 11
syscall_def draw_primitive_line, 12
syscall_def draw_primitive_rect, 13
syscall_def draw_primitive_char, 14
syscall_def draw_primitive_text, 15

//GPU commands
syscall_def gpu_flush_data, 20
syscall_def gpu_screen_size, 21
syscall_def gpu_char_size, 21

syscall_def process_sleep, 30
syscall_def process_yield, 31
// syscall_def process_await, 32
syscall_def process_terminate, 33

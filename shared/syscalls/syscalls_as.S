.macro syscall_def name:req, id:req
.global \name
.type \name, %function
\name:
    svc #\id
    ret
.endm

syscall_def malloc, 0
syscall_def free, 1

syscall_def printl, 3

syscall_def read_key, 5
syscall_def register_shortcut, 6
syscall_def read_shortcut, 7

//Primitives
syscall_def clear_screen, 10
syscall_def draw_primitive_pixel, 11
syscall_def draw_primitive_line, 12
syscall_def draw_primitive_rect, 13
syscall_def draw_primitive_char, 14
syscall_def draw_primitive_string, 15

//GPU commands
syscall_def gpu_flush_data, 20
syscall_def gpu_screen_size, 21
syscall_def gpu_char_size, 21

syscall_def sleep, 30
syscall_def process_yield, 31
// syscall_def process_await, 32
syscall_def halt, 33

//Time commands
syscall_def get_time, 40

//Network commands
syscall_def bind_port, 51
syscall_def unbind_port, 52
syscall_def send_packet, 53
syscall_def read_packet, 54
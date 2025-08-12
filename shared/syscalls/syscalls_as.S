#include "syscall_codes.h"

.macro syscall_def name:req, id:req
.global \name
.type \name, %function
\name:
    svc #\id
    ret
.endm

syscall_def malloc, MALLOC_CODE
syscall_def free, FREE_CODE

syscall_def printl, PRINTL_CODE

syscall_def read_key, READ_KEY_CODE
syscall_def register_shortcut, REGISTER_SHORTCUT_CODE
syscall_def read_shortcut, READ_SHORTCUT_CODE

syscall_def clear_screen, CLEAR_SCREEN_CODE
syscall_def draw_primitive_pixel, DRAW_PRIMITIVE_PIXEL_CODE
syscall_def draw_primitive_line, DRAW_PRIMITIVE_LINE_CODE
syscall_def draw_primitive_rect, DRAW_PRIMITIVE_RECT_CODE
syscall_def draw_primitive_char, DRAW_PRIMITIVE_CHAR_CODE
syscall_def draw_primitive_string, DRAW_PRIMITIVE_STRING_CODE

syscall_def gpu_flush_data, GPU_FLUSH_DATA_CODE
syscall_def gpu_screen_size, GPU_SCREEN_SIZE_CODE
syscall_def gpu_char_size, GPU_CHAR_SIZE_CODE

syscall_def sleep, SLEEP_CODE
syscall_def process_yield, PROCESS_YIELD_CODE
syscall_def halt, HALT_CODE

syscall_def get_time, GET_TIME_CODE

syscall_def bind_port, BIND_PORT_CODE
syscall_def unbind_port, UNBIND_PORT_CODE
syscall_def send_packet, SEND_PACKET_CODE
syscall_def read_packet, READ_PACKET_CODE
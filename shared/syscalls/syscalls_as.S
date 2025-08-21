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

syscall_def request_draw_ctx, REQUEST_DRAW_CTX_CODE
syscall_def commit_draw_ctx, GPU_FLUSH_DATA_CODE

syscall_def gpu_char_size, GPU_CHAR_SIZE_CODE

syscall_def sleep, SLEEP_CODE
syscall_def process_yield, PROCESS_YIELD_CODE
syscall_def halt, HALT_CODE

syscall_def get_time, GET_TIME_CODE

syscall_def bind_port, BIND_PORT_CODE
syscall_def unbind_port, UNBIND_PORT_CODE
syscall_def send_packet, SEND_PACKET_CODE
syscall_def read_packet, READ_PACKET_CODE
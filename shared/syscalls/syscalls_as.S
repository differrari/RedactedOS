#include "syscall_codes.h"

.macro syscall_def name:req, id:req
.global \name
.type \name, %function
\name:
    svc #\id
    ret
.endm

#ifndef KERNEL
syscall_def malloc, MALLOC_CODE
syscall_def free, FREE_CODE
#endif

syscall_def printl, PRINTL_CODE

syscall_def read_key, READ_KEY_CODE
syscall_def read_event, READ_EVENT_CODE
syscall_def register_shortcut, REGISTER_SHORTCUT_CODE
syscall_def read_shortcut, READ_SHORTCUT_CODE
syscall_def get_mouse_status, GET_MOUSE_STATUS_CODE

syscall_def request_draw_ctx, REQUEST_DRAW_CTX_CODE
syscall_def commit_draw_ctx, GPU_FLUSH_DATA_CODE
syscall_def resize_draw_ctx, RESIZE_DRAW_CTX_CODE

syscall_def gpu_char_size, GPU_CHAR_SIZE_CODE

syscall_def sleep, SLEEP_CODE
syscall_def process_yield, PROCESS_YIELD_CODE
syscall_def halt, HALT_CODE
syscall_def exec, EXEC_CODE

syscall_def get_time, GET_TIME_CODE

syscall_def socket_create, SOCKET_CREATE_CODE
syscall_def socket_bind, SOCKET_BIND_CODE
syscall_def socket_connect, SOCKET_CONNECT_CODE
syscall_def socket_listen, SOCKET_LISTEN_CODE
syscall_def socket_accept, SOCKET_ACCEPT_CODE
syscall_def socket_send, SOCKET_SEND_CODE
syscall_def socket_receive, SOCKET_RECEIVE_CODE
syscall_def socket_close, SOCKET_CLOSE_CODE

syscall_def fopen, FILE_OPEN_CODE
syscall_def fread, FILE_READ_CODE
syscall_def fwrite, FILE_WRITE_CODE
syscall_def fclose, FILE_CLOSE_CODE
syscall_def dir_list, DIR_LIST_CODE

syscall_def icmp_ping_sys, ICMP_PING_CODE
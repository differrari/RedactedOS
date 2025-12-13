#pragma once

#define MALLOC_CODE                 0
#define FREE_CODE                   1

#define PRINTL_CODE                 3

#define READ_KEY_CODE               10
#define REGISTER_SHORTCUT_CODE      11
#define READ_SHORTCUT_CODE          12
#define GET_MOUSE_STATUS_CODE       13
#define READ_EVENT_CODE             14

#define REQUEST_DRAW_CTX_CODE       20
#define GPU_FLUSH_DATA_CODE         21
#define GPU_CHAR_SIZE_CODE          23
#define RESIZE_DRAW_CTX_CODE        24

#define SLEEP_CODE                  30
#define HALT_CODE                   31
#define EXEC_CODE                   32

#define GET_TIME_CODE               40

#define SOCKET_CREATE_CODE          50
#define SOCKET_BIND_CODE            51
#define SOCKET_CONNECT_CODE         52
#define SOCKET_LISTEN_CODE          53
#define SOCKET_ACCEPT_CODE          54
#define SOCKET_SEND_CODE            55
#define SOCKET_RECEIVE_CODE         56
#define SOCKET_CLOSE_CODE           57

#define FILE_OPEN_CODE              60
#define FILE_READ_CODE              61
#define FILE_WRITE_CODE             62
#define FILE_CLOSE_CODE             63
#define FILE_SIMPLE_READ_CODE       64
#define FILE_SIMPLE_WRITE_CODE      65

#define DIR_LIST_CODE               70
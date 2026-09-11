#pragma once

#include "socket_core.h"

#ifdef __cplusplus
extern "C" {
#endif

socket_impl_t socket_ctrl_create(ksocket_t* owner, const SocketOptions* extra);
void socket_destroy_ctrl(socket_impl_t sh);
int32_t socket_close_ctrl(socket_impl_t sh);
int32_t socket_setopt_ctrl(socket_impl_t sh, int32_t opt, const void* value, uint32_t len);
int32_t socket_getopt_ctrl(socket_impl_t sh, int32_t opt, void* value, uint32_t* len);
int64_t socket_send_ctrl(socket_impl_t sh, const void* buf, uint64_t len);
int64_t socket_recv_ctrl(socket_impl_t sh, void* buf, uint64_t len);

#ifdef __cplusplus
}
#endif

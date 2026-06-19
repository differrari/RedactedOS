#pragma once

#include "socket_core.h"

#ifdef __cplusplus
extern "C" {
#endif

socket_impl_t socket_special_create(ksocket_t* owner, const SocketOptions* extra);
void socket_destroy_special(socket_impl_t sh);
int32_t socket_close_special(socket_impl_t sh);
int32_t socket_setopt_special(socket_impl_t sh, int32_t opt, const void* value, uint32_t len);
int32_t socket_getopt_special(socket_impl_t sh, int32_t opt, void* value, uint32_t* len);
int64_t socket_send_special(socket_impl_t sh, const void* buf, uint64_t len);
int64_t socket_recv_special(socket_impl_t sh, void* buf, uint64_t len);

#ifdef __cplusplus
}
#endif

#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* socket_impl_t;
typedef struct ksocket ksocket_t;

typedef void (*socket_impl_destroy_fn)(socket_impl_t impl);
typedef int32_t (*socket_impl_close_fn)(socket_impl_t impl);
typedef int32_t (*socket_impl_setopt_fn)(socket_impl_t impl, int32_t opt, const void* value, uint32_t len);
typedef int32_t (*socket_impl_getopt_fn)(socket_impl_t impl, int32_t opt, void* value, uint32_t* len);

#define SOCKET_MAX_OPEN 2048
#define SOCKET_HANDLE_INDEX_BITS 20

bool socket_core_alloc(protocol_t protocol, uint16_t pid, ksocket_t** out_socket);
bool socket_core_attach_impl(ksocket_t* socket, socket_impl_t impl, socket_impl_destroy_fn destroy, socket_impl_close_fn close, socket_impl_setopt_fn setopt, socket_impl_getopt_fn getopt);
ksocket_t* socket_core_get(socket_handle_t handle, uint16_t pid);
void socket_core_ref(ksocket_t* socket);
void socket_core_put(ksocket_t* socket);
int32_t socket_core_close_handle(socket_handle_t handle, uint16_t pid);
int32_t socket_core_close_socket(ksocket_t* socket);
int32_t socket_core_set_option(ksocket_t* socket, int32_t opt, const void* value, uint32_t len);
int32_t socket_core_get_option(ksocket_t* socket, int32_t opt, void* value, uint32_t* len);
int32_t socket_common_options_set(SocketOptions* opts, int32_t opt, const void* value, uint32_t len);
int32_t socket_common_options_get(const SocketOptions* opts, int32_t opt, void* value, uint32_t* len);

socket_impl_t socket_core_impl(ksocket_t* socket);
protocol_t socket_core_protocol(const ksocket_t* socket);
uint16_t socket_core_pid(const ksocket_t* socket);
bool socket_core_is_closing(const ksocket_t* socket);
socket_handle_t socket_core_export_handle(const ksocket_t* socket);

#ifdef __cplusplus
}
#endif

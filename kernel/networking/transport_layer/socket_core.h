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

bool socket_core_alloc(Socket_Role role, protocol_t protocol, uint16_t pid, ksocket_t** out_socket);
bool socket_core_attach_impl(ksocket_t* socket, socket_impl_t impl, socket_impl_destroy_fn destroy, socket_impl_close_fn close, socket_impl_setopt_fn setopt, socket_impl_getopt_fn getopt, SocketHandle* out_handle); 
ksocket_t* socket_core_get(const SocketHandle* handle, uint16_t pid);
void socket_core_ref(ksocket_t* socket);
void socket_core_put(ksocket_t* socket);
int32_t socket_core_close_handle(SocketHandle* handle, uint16_t pid);
int32_t socket_core_close_socket(ksocket_t* socket);
int32_t socket_core_set_option(ksocket_t* socket, int32_t opt, const void* value, uint32_t len);
int32_t socket_core_get_option(ksocket_t* socket, int32_t opt, void* value, uint32_t* len);

socket_impl_t socket_core_impl(ksocket_t* socket);
protocol_t socket_core_protocol(const ksocket_t* socket);
Socket_Role socket_core_role(const ksocket_t* socket);
uint16_t socket_core_pid(const ksocket_t* socket);
uint32_t socket_core_id(const ksocket_t* socket);
uint32_t socket_core_generation(const ksocket_t* socket);
bool socket_core_is_closing(const ksocket_t* socket);
void socket_core_export_handle(const ksocket_t* socket, SocketHandle* out_handle);

#ifdef __cplusplus
}
#endif

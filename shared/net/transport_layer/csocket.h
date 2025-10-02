#pragma once

#include "types.h"
#include "net/network_types.h"

bool create_socket(Socket_Role role, protocol_t protocol, uint16_t pid, SocketHandle *out_handle);
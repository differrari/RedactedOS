#pragma once

#include "types.h"
#include "networking/transport_layer/csocket.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ntp_is_running(void);

int ntp_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif

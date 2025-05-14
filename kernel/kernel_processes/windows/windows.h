#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "process.h"

process_t* start_windows();

void pause_window_draw();
void resume_window_draw();

#ifdef __cplusplus
}
#endif
#pragma once

#include "files/system_module.h"
#include "environment/env_types.h"

extern system_module environment_module;

typedef struct {
    env_display_type display_type;
    env_behavior behavior;
    buffer data;
    buffer structure;
    buffer display_buf;
} environment_data;

typedef enum { env_type_none, env_type_display, env_type_behavior, env_type_data, env_type_structure } env_data_types;

void register_environment(u16 procid);
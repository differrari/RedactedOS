#pragma once

#include "files/system_module.h"
#include "environment/env_types.h"

extern system_module environment_module;

typedef struct {
    env_config config;
    buffer data;
    buffer structure;
    buffer config_buf;
} environment_data;

typedef enum { env_type_none, env_type_config, env_type_data, env_type_structure } env_data_types;

void register_environment(u16 procid);
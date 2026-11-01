#pragma once

#include "resources.h"

typedef u32 mutex_owner;

mutex_owner new_mutex_owner();

bool lock_mutex(mutex_owner owner, resource r);
bool unlock_mutex(mutex_owner owner, resource r);
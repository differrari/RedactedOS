#pragma once

#include "filesystem/modules/module_loader.h"

#include "clipboard/clipboard.h"

static inline bool load_util_mods(){
    return 
        load_module(&clipboard_mod) && 
    true;
}
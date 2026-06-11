#pragma once

#include "filesystem/modules/module_loader.h"

#include "clipboard/clipboard.h"
#include "language_support/language.h"
#include "random/random.h"

static inline bool load_util_mods(){
    return 
        load_module(&clipboard_mod) && 
        load_module(&language_mod) &&
        load_module(&rng_module) &&
    true;
}
#pragma once

#include "types.h"
#include "sysregs.h"

#ifdef __cplusplus
extern "C" {
#endif

bool kva_is_dmap(uintptr_t va);
uintptr_t dmap_pa_to_kva(paddr_t pa);
paddr_t dmap_kva_to_pa(uintptr_t va);
void* pt_pa_to_va(paddr_t pa);
paddr_t pt_va_to_pa(const void* va);
#ifdef __cplusplus
}
#endif
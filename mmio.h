#ifndef MMIO_H
#define MMIO_H

#include "types.h"


void mmio_write(uint64_t reg, uint64_t data);
uint64_t mmio_read(uint64_t reg);
void mmio_write_barrier();

#endif
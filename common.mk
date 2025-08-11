ARCH       ?= aarch64-none-elf
CC         := $(ARCH)-gcc
CXX        := $(ARCH)-g++
LD         := $(ARCH)-ld
AR         := $(ARCH)-ar
OBJCOPY    := $(ARCH)-objcopy

COMMON_FLAGS  ?= -ffreestanding -nostdlib -fno-exceptions -fno-unwind-tables \
                 -fno-asynchronous-unwind-tables -g -O0 -Wall -Wextra \
                 -Wno-unused-parameter -Wno-address-of-packed-member \
                 -mcpu=cortex-a72

CFLAGS_BASE   ?= $(COMMON_FLAGS) -std=c17
CXXFLAGS_BASE ?= $(COMMON_FLAGS) -fno-rtti
LDFLAGS_BASE  ?=

LOAD_ADDR      ?= 0x41000000
XHCI_CTX_SIZE  ?= 32
QEMU           ?= true
MODE           ?= virt

ifeq ($(V), 1)
  VAR  = $(AR)
  VAS  = $(CC)
  VCC  = $(CC)
  VCXX = $(CXX)
  VLD  = $(LD)
else
  VAR  = @echo "  [AR]   $@" && $(AR)
  VAS  = @echo "  [AS]   $@" && $(CC)
  VCC  = @echo "  [CC]   $@" && $(CC)
  VCXX = @echo "  [CXX]  $@" && $(CXX)
  VLD  = @echo "  [LD]   $@" && $(LD)
endif

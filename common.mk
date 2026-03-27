ARCH       ?= aarch64-none-elf-
ifeq "$(origin CC)" "default"
CC         := $(ARCH)gcc
endif
ifeq "$(origin CXX)" "default"
CXX        := $(ARCH)g++
endif
LD         := $(ARCH)ld
AR         := $(ARCH)ar
OBJCOPY    := $(ARCH)objcopy

BUILD_DIR := ./build

COMMON_FLAGS  ?= -ffreestanding -nostdlib -fno-exceptions -fno-unwind-tables \
                 -fno-asynchronous-unwind-tables -g -O0 -Wall -Wextra \
                 -Wno-unused-parameter -Wno-address-of-packed-member \
                 #-Werror \
                 -Wno-unused-function

ifeq ($(ARCH), aarch64-none-elf-)
COMMON_FLAGS += -mcpu=cortex-a72 -Wno-error=sized-deallocation 
endif

CFLAGS_BASE   ?= $(COMMON_FLAGS) -std=c99
CXXFLAGS_BASE ?= $(COMMON_FLAGS) -fno-rtti
LDFLAGS_BASE  ?=

LOAD_ADDR      ?= 0x41000000
XHCI_CTX_SIZE  ?= 32
QEMU           ?= true
MODE           ?= virt
TEST           ?= false

ifeq ($(V), 1)
  VAR  = $(AR)
  VAS  = $(CC)
  VCC  = $(CC)
  VCXX = $(CXX)
  VLD  = $(LD)
else
  VAR  = @echo "  [AR]   $<" && $(AR)
  VAS  = @echo "  [AS]   $<" && $(CC)
  VCC  = @echo "  [CC]   $<" && $(CC)
  VCXX = @echo "  [CXX]  $<" && $(CXX)
  VLD  = @echo "  [LD]   $<" && $(LD)
endif

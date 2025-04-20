# Compiler and Linker
ARCH= aarch64-none-elf
CC = $(ARCH)-gcc
LD = $(ARCH)-ld
OBJCOPY = $(ARCH)-objcopy

# Compiler and Linker Flags
CFLAGS = -nostdlib -ffreestanding -Wall -Wextra -mcpu=cortex-a72 -I. -Wno-unused-parameter
LDFLAGS = -T $(shell ls *.ld)

C_SRC = $(shell find . -name '*.c')
ASM_SRC = $(shell find . -name '*.S')
CPP_SRC = $(shell find . -name '*.cpp')
OBJ = $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o) $(ASM_SRC:.S=.o)

# Output File
TARGET = kernel.img

# Build Rules
all: $(TARGET)

$(TARGET): $(OBJ)
	$(LD) $(LDFLAGS) -o kernel.bin $(OBJ)
	$(OBJCOPY) -O binary kernel.bin ../$(TARGET)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) kernel.bin ../$(TARGET)
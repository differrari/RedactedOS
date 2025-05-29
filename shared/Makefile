# Compiler
ARCH= aarch64-none-elf
CC = $(ARCH)-gcc
AR = $(ARCH)-ar
OBJCOPY = $(ARCH)-objcopy

# Compiler Flags
CFLAGS = -g -O0 -nostdlib -nolibc -ffreestanding -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -mcpu=cortex-a72 -I. -I../kernel -Wno-unused-parameter

# Source and Object Files
C_SRC = $(shell find . -name '*.c')
CPP_SRC = $(shell find . -name '*.cpp')
ASM_SRC = $(shell find . -name '*.S')
OBJ = $(C_SRC:.c=.o) $(ASM_SRC:.S=.o) $(CPP_SRC:.cpp=.o)

# Output File
TARGET = libshared.a

# Build Rules
all: $(TARGET)

$(TARGET): $(OBJ)
	$(AR) rcs $(TARGET) $(OBJ)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -fno-rtti -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
# Compiler
ARCH= aarch64-none-elf
CC = $(ARCH)-gcc
AR = $(ARCH)-ar
OBJCOPY = $(ARCH)-objcopy

# Compiler Flags
CFLAGS = -g -O0 -nostdlib -ffreestanding -Wall -Wextra -mcpu=cortex-a72 -I. -I../shared -Wno-unused-parameter

# Source and Object Files
C_SRC = $(shell find . -name '*.c')
CPP_SRC = $(shell find . -name '*.cpp')
OBJ = $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o)

# Output File
TARGET = libuser.a

# Build Rules
all: $(TARGET)

$(TARGET): $(OBJ)
	# $(LD) $(LDFLAGS) -o ../$(ELF) $(OBJ)  ../user/libuser.a
	$(AR) rcs $(TARGET) ../shared/libshared.a $(OBJ)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
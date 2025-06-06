ARCH= aarch64-none-elf
CC = $(ARCH)-gcc
LD = $(ARCH)-ld
OBJCOPY = $(ARCH)-objcopy

CFLAGS = -g -O0 -nostdlib -ffreestanding -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -mcpu=cortex-a72 -I. -I../shared -I../user
LDFLAGS = -T $(shell ls *.ld)

C_SRC = $(shell find . -name '*.c')
ASM_SRC = $(shell find . -name '*.S')
CPP_SRC = $(shell find . -name '*.cpp')
OBJ = $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o) $(ASM_SRC:.S=.o)

ELF = kernel.elf

all: kernel.img

kernel.img: ../shared/libshared.a ../user/libuser.a $(OBJ)
	$(LD) $(LDFLAGS) -o ../$(ELF) $(OBJ) ../user/libuser.a ../shared/libshared.a
	$(OBJCOPY) -O binary ../$(ELF) ../$@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -fno-rtti -c $< -o $@

clean:
	rm -f $(OBJ) ../$(ELF) ../kernel.img kernel.img

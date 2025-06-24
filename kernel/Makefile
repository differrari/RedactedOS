ARCH= aarch64-none-elf
CC = $(ARCH)-gcc
LD = $(ARCH)-ld
OBJCOPY = $(ARCH)-objcopy

CFLAGS = -g -O0 -nostdlib -ffreestanding -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -mcpu=cortex-a72 -I. -I../shared -I../user

LDFLAGS = -T $(shell ls *.ld) --defsym=LOAD_ADDR=$(LOAD_ADDR)

C_SRC = $(shell find . -name '*.c')
ASM_SRC = $(shell find . -name '*.S')
CPP_SRC = $(shell find . -name '*.cpp')
OBJ = $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o) $(ASM_SRC:.S=.o)
OBJL = $(filter-out ./boot.o, $(OBJ))

TARGET = kernel.img
ELF = kernel.elf

all: $(TARGET)

$(TARGET): ../shared/libshared.a $(OBJ)
	$(LD) $(LDFLAGS) -o ../$(ELF) $(OBJL) ../shared/libshared.a
	$(OBJCOPY) -O binary ../$(ELF) ../$(TARGET)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -fno-rtti -c $< -o $@

clean:
	rm -f $(shell find . -name '*.o') ../$(ELF) ../$(TARGET) $(TARGET)
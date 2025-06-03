ARCH= aarch64-none-elf
CC = $(ARCH)-gcc
LD = $(ARCH)-ld
OBJCOPY = $(ARCH)-objcopy

CFLAGS = -g -O0 -nostdlib -ffreestanding -Wall -Wextra -mcpu=cortex-a72 -I. -I../shared -Wno-unused-parameter
LDFLAGS = -T $(shell ls *.ld)

C_SRC = $(shell find . -name '*.c')
CPP_SRC = $(shell find . -name '*.cpp')
OBJ = $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o)

NAME = $(shell basename "$$PWD")
TARGET = $(NAME).bin
ELF = $(NAME).elf
LOCATION = ../fs/redos/user/

all: $(TARGET)

$(TARGET): $(OBJ)
	$(LD) $(LDFLAGS) -o $(LOCATION)$(ELF) $(OBJ) ../shared/libshared.a
	$(OBJCOPY) -O binary $(LOCATION)$(ELF) $(LOCATION)$(TARGET)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
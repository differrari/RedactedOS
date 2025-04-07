CFLAGS = -nostdlib -ffreestanding -c -mcpu=arm926ej-s
#-Wall -Wextra
LDFLAGS = -T linker.ld

SRC = boot.s kernel.c uart.c framebuffer.c

all: kernel.img

kernel.img: $(SRC)
	arm-none-eabi-as -mcpu=arm926ej-s -g boot.s -o boot.o
	arm-none-eabi-gcc $(CFLAGS) -g uart.c -o uart.o
	arm-none-eabi-gcc $(CFLAGS) -g kernel.c -o kernel.o
	arm-none-eabi-ld $(LDFLAGS) kernel.o uart.o boot.o -o kernel.elf
	arm-none-eabi-objcopy -O binary kernel.elf kernel.img

clean:
	rm -f *.o *.elf *.img
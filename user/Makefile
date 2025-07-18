#user
CFLAGS  := $(CFLAGS_BASE) -I. -I../shared -Wno-unused-parameter
LDFLAGS := -T $(shell ls *.ld)

C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o)

NAME     := $(notdir $(CURDIR))
ELF      := $(NAME).elf
TARGET   := $(NAME).bin
LOCATION := ../fs/redos/user/

.PHONY: all clean

all: $(LOCATION)$(TARGET)

$(LOCATION)$(TARGET): $(OBJ)
	$(LD) $(LDFLAGS) -o $(LOCATION)$(ELF) $(OBJ) ../shared/libshared.a
	$(OBJCOPY) -O binary $(LOCATION)$(ELF) $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -fno-rtti -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

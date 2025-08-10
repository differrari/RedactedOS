#user

include ../common.mk

CPPFLAGS := -I. -I../shared
CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)
LDFLAGS  := -T $(shell ls *.ld)

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o)
DEP     := $(C_SRC:.c=.d) $(CPP_SRC:.cpp=.d)

NAME     := $(notdir $(CURDIR))
ELF      := $(NAME).elf
TARGET   := $(NAME).bin
LOCATION := ../fs/redos/user/

.PHONY: all clean

all: $(LOCATION)$(TARGET)

$(LOCATION)$(TARGET): $(OBJ)
	$(VLD) $(LDFLAGS) -o $(LOCATION)$(ELF) $(OBJ) ../shared/libshared.a
	$(OBJCOPY) -O binary $(LOCATION)$(ELF) $@

%.o: %.S
	$(VAS) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(VCC) $(CFLAGS) -c -MMD -MP $< -o $@

%.o: %.cpp
	$(VCXX) $(CXXFLAGS) -c -MMD -MP $< -o $@

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(TARGET)

-include $(DEP)

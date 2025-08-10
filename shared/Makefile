#shared
CFLAGS  := $(CFLAGS_BASE) -I. -I../kernel -Wno-unused-parameter

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
ASM_SRC := $(shell find . -name '*.S')
OBJ     := $(C_SRC:.c=.o) $(ASM_SRC:.S=.o) $(CPP_SRC:.cpp=.o)
DEP     := $(C_SRC:.c=.d) $(ASM_SRC:.S=.d) $(CPP_SRC:.cpp=.d)

TARGET  := libshared.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(AR) rcs $@ $^

%.o: %.S
	$(CC) $(CFLAGS) -c -MMD -MP $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(CONLY_FLAGS_BASE) -c -MMD -MP $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -fno-rtti -c -MMD -MP $< -o $@

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(TARGET)

-include $(DEP)

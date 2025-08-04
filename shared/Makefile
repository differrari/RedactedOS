#shared
CFLAGS  := $(CFLAGS_BASE) -I. -I../kernel -Wno-unused-parameter

CLEAN_OBJS := $(shell find . -name '*.o')
C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
ASM_SRC := $(shell find . -name '*.S')
OBJ     := $(C_SRC:.c=.o) $(ASM_SRC:.S=.o) $(CPP_SRC:.cpp=.o)

TARGET  := libshared.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(AR) rcs $@ $^

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(CONLY_FLAGS_BASE) -c $< -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -fno-rtti -c $< -o $@

clean:
	rm -f $(CLEAN_OBJS) $(TARGET)

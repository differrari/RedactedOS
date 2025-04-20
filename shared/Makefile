# Compiler
ARCH= aarch64-none-elf
CC = $(ARCH)-gcc
CXX = $(ARCH)-g++

# Flags
CFLAGS = -nostdlib -ffreestanding -Wall -Wextra -mcpu=cortex-a72 -I. -Wno-unused-parameter
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti

C_SRC = $(shell find . -name '*.c')
CPP_SRC = $(shell find . -name '*.cpp')
OBJ = $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o)

# Output
TARGET = libshared.a

all: $(TARGET)

$(TARGET): $(OBJ)
	$(AR) rcs $(TARGET) $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

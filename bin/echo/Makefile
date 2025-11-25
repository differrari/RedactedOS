# Simple Makefile for echo command
TARGET = echo
SRCS = echo.c
OBJS = $(SRCS:.c=.o)

CC = gcc
CFLAGS = -Wall -Wextra -O2

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

echo.o: echo.c

clean:
	rm -f $(TARGET) $(OBJS)

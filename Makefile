CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -pthread
SRC = $(wildcard src/*.c src/core/*.c src/net/*.c)
OUT = build/velora

all:
	mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)
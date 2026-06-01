CC = gcc
CFLAGS = -Wall -Wextra -Iinclude
SRC = src/main.c
OUT = build/velora

all:
	mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)
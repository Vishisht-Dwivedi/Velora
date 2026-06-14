CC = gcc

DEBUG_FLAGS = -DVR_DEBUG -g
RELEASE_FLAGS = -O3

CFLAGS = -Wall -Wextra -Iinclude -pthread

SRC = $(wildcard src/*.c src/event_loop/*.c src/core/*.c src/net/*.c)

OUT = build/velora

debug:
	mkdir -p build
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(SRC) -o $(OUT)

release:
	mkdir -p build
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(SRC) -o $(OUT)

all: debug
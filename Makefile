CC = gcc

DEBUG_FLAGS = -DVR_DEBUG -g
RELEASE_FLAGS = -O3

CFLAGS = -Wall -Wextra -Iinclude -pthread

SRC = $(wildcard src/*.c src/event_loop/*.c src/core/*.c src/net/*.c src/conn/*.c src/shared/*.c src/protocol/*.c)

OUT = build/velora
TEST_PROTOCOL = build/protocol_framing

.PHONY: debug release test-protocol mtcp-af_xdp

debug:
	mkdir -p build
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(SRC) -o $(OUT)

release:
	mkdir -p build
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(SRC) -o $(OUT)

all: debug

test-protocol:
	mkdir -p build
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) tests/protocol_framing.c src/protocol/packet.c src/protocol/parser.c src/protocol/protocol.c src/conn/ring_buffer.c src/core/logger.c src/core/error.c -o $(TEST_PROTOCOL)
	./$(TEST_PROTOCOL)

mtcp-af_xdp:
	bash dependencies/build-mtcp-af_xdp.sh
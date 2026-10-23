CC = gcc

DEBUG_FLAGS = -DVR_DEBUG -g
RELEASE_FLAGS = -O3

CFLAGS = -Wall -Wextra -Iinclude -pthread

SRC = $(filter-out src/net/mtcp_runtime.c,$(wildcard src/*.c src/event_loop/*.c src/core/*.c src/net/*.c src/conn/*.c src/shared/*.c src/protocol/*.c))

OUT = build/velora
TEST_PROTOCOL = build/protocol_framing
TEST_BUFFERS = build/ring_connection
MTCP_SOURCE = .deps/mtcp-af_xdp/source
MTCP_CPPFLAGS = -I$(MTCP_SOURCE)/mtcp/include
MTCP_LDLIBS = $(MTCP_SOURCE)/mtcp/lib/libmtcp.a \
	$(MTCP_SOURCE)/afxdp/libbpf/src/libbpf.a -lelf -lnuma -lz -lm -pthread \
	-Wl,--allow-multiple-definition
MTCP_RUNTIME_SRCS = src/net/mtcp_runtime.c src/conn/conn.c \
	src/conn/ring_buffer.c src/shared/utilities.c src/protocol/parser.c \
	src/protocol/packet.c src/protocol/protocol.c src/core/logger.c src/core/error.c
MTCP_RUNTIME_SMOKE = build/mtcp_runtime_smoke

.PHONY: debug release test-protocol test-buffers test-af-xdp-packet-io mtcp-af_xdp mtcp-runtime-smoke

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

test-buffers:
	mkdir -p build
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) tests/ring_connection.c src/conn/conn.c src/conn/ring_buffer.c src/shared/utilities.c src/core/logger.c src/core/error.c -o $(TEST_BUFFERS)
	./$(TEST_BUFFERS)

test-af-xdp-packet-io:
	@test -f $(MTCP_SOURCE)/mtcp/src/include/afxdp_packet_io.h || { echo "Run make mtcp-af_xdp first" >&2; exit 1; }
	@test_binary=$$(mktemp /tmp/velora-af-xdp-packet-io.XXXXXX); \
	$(CC) -std=gnu11 -Wall -Wextra -Werror \
		-I$(MTCP_SOURCE)/mtcp/src/include \
		-I$(MTCP_SOURCE)/afxdp/libbpf/src/build/usr/include \
		-I$(MTCP_SOURCE)/afxdp/libbpf/src \
		-I$(MTCP_SOURCE)/afxdp/libbpf/include/uapi \
		-I$(MTCP_SOURCE)/afxdp/libbpf/include \
		tests/afxdp_packet_io.c -o "$$test_binary" && "$$test_binary"

mtcp-af_xdp:
	bash dependencies/build-mtcp-af_xdp.sh

mtcp-runtime-smoke:
	mkdir -p build
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(MTCP_CPPFLAGS) tests/mtcp_runtime_smoke.c $(MTCP_RUNTIME_SRCS) $(MTCP_LDLIBS) -o $(MTCP_RUNTIME_SMOKE)

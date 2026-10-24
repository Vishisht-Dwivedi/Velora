CC = gcc

DEBUG_FLAGS = -DVR_DEBUG -g
RELEASE_FLAGS = -O3

CFLAGS = -Wall -Wextra -Iinclude -pthread

SRC = $(filter-out src/mtcp_main.c src/net/mtcp_runtime.c,$(wildcard src/*.c src/event_loop/*.c src/core/*.c src/net/*.c src/conn/*.c src/shared/*.c src/protocol/*.c))

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
MTCP_SERVER = build/velora-mtcp

.PHONY: debug release test-protocol test-buffers test-af-xdp-packet-io test-mtcp-client test-mtcp-integration mtcp-af_xdp mtcp-runtime-smoke velora-mtcp

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

test-mtcp-client:
	python3 tests/mtcp_protocol_client_test.py

test-mtcp-integration:
	@: "$${MTCP_CONFIG:?Set MTCP_CONFIG to the mTCP config file}"
	@: "$${MTCP_BIND_IPV4:?Set MTCP_BIND_IPV4 to the server bind address}"
	@: "$${MTCP_PORT:?Set MTCP_PORT to the server port}"
	@: "$${MTCP_CLIENT_HOST:?Set MTCP_CLIENT_HOST to the client destination}"
	tests/mtcp_integration_smoke.sh "$$MTCP_CONFIG" "$$MTCP_BIND_IPV4" "$$MTCP_PORT" "$$MTCP_CLIENT_HOST" "$${MTCP_STARTUP_TIMEOUT:-15}"

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

velora-mtcp:
	@test -f $(MTCP_SOURCE)/mtcp/include/mtcp_api.h || { echo "Missing mTCP public header: $(MTCP_SOURCE)/mtcp/include/mtcp_api.h" >&2; exit 1; }
	@test -f $(MTCP_SOURCE)/mtcp/include/mtcp_epoll.h || { echo "Missing mTCP public header: $(MTCP_SOURCE)/mtcp/include/mtcp_epoll.h" >&2; exit 1; }
	@test -f $(MTCP_SOURCE)/mtcp/lib/libmtcp.a || { echo "Missing mTCP library: $(MTCP_SOURCE)/mtcp/lib/libmtcp.a; prepare the pinned dependency without running its patching build script" >&2; exit 1; }
	@test -f $(MTCP_SOURCE)/afxdp/libbpf/src/libbpf.a || { echo "Missing libbpf library: $(MTCP_SOURCE)/afxdp/libbpf/src/libbpf.a; prepare the pinned dependency without running its patching build script" >&2; exit 1; }
	mkdir -p build
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(MTCP_CPPFLAGS) src/mtcp_main.c $(MTCP_RUNTIME_SRCS) $(MTCP_LDLIBS) -o $(MTCP_SERVER)

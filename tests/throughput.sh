#!/bin/bash

HOST="${1:-127.0.0.1}"
PORT="${2:-22409}"
CLIENTS="${3:-10000}"
PAYLOAD_SIZE="${4:-64}"
THREADS="${5:-16}"
DURATION="${6:-30}"

python3 - "$HOST" "$PORT" "$CLIENTS" "$PAYLOAD_SIZE" "$THREADS" "$DURATION" <<'PY'
import socket
import struct
import sys
import time
import threading

HOST = sys.argv[1]
PORT = int(sys.argv[2])
CLIENTS = int(sys.argv[3])
PAYLOAD_SIZE = int(sys.argv[4])
THREADS = int(sys.argv[5])
DURATION = float(sys.argv[6])

MAGIC = 0x5789
VERSION = 1

CONNECT = 1
CONNECT_ACK = 2
PUBLISH = 8

HEADER_LEN = 8

# ---------------------------------------------------------------------
# Packet construction
# ---------------------------------------------------------------------

def packet(pkt_type, stream_id=0, flags=0, payload=b""):
    return struct.pack(
        "!HBBBBH",
        MAGIC,
        VERSION,
        pkt_type,
        stream_id,
        flags,
        len(payload),
    ) + payload


PAYLOAD = bytes(i % 251 for i in range(PAYLOAD_SIZE))

CONNECT_PACKET = packet(CONNECT)

PUBLISH_PACKET = packet(
    PUBLISH,
    stream_id=0,
    payload=PAYLOAD,
)

PACKET_SIZE = len(PUBLISH_PACKET)


# ---------------------------------------------------------------------
# Global statistics
#
# Workers accumulate locally and periodically flush here.
# This avoids taking the lock for every message.
# ---------------------------------------------------------------------

stats_lock = threading.Lock()

total_messages = 0
total_bytes = 0
total_failures = 0

active_connections = 0
failed_connections = 0


def flush_stats(messages, bytes_sent, failures):

    global total_messages
    global total_bytes
    global total_failures

    if messages == 0 and bytes_sent == 0 and failures == 0:
        return

    with stats_lock:
        total_messages += messages
        total_bytes += bytes_sent
        total_failures += failures


def get_stats():

    with stats_lock:
        return (
            total_messages,
            total_bytes,
            total_failures,
            active_connections,
            failed_connections,
        )


# ---------------------------------------------------------------------
# Receive exactly N bytes
# ---------------------------------------------------------------------

def recv_exact(sock, n):

    data = bytearray()

    while len(data) < n:

        chunk = sock.recv(n - len(data))

        if not chunk:
            raise ConnectionError("peer closed connection")

        data.extend(chunk)

    return bytes(data)


# ---------------------------------------------------------------------
# CONNECT -> CONNECT_ACK
# ---------------------------------------------------------------------

def establish(sock):

    sock.sendall(CONNECT_PACKET)

    header = recv_exact(sock, HEADER_LEN)

    magic, version, pkt_type, stream_id, flags, payload_len = struct.unpack(
        "!HBBBBH",
        header,
    )

    if magic != MAGIC:
        raise RuntimeError(
            f"bad magic: 0x{magic:04x}"
        )

    if version != VERSION:
        raise RuntimeError(
            f"bad version: {version}"
        )

    if pkt_type != CONNECT_ACK:
        raise RuntimeError(
            f"expected CONNECT_ACK, got {pkt_type}"
        )

    if payload_len:
        recv_exact(sock, payload_len)


# ---------------------------------------------------------------------
# Worker
# ---------------------------------------------------------------------

def worker(worker_id, client_ids, ready_event, start_event, stop_event):

    global active_connections
    global failed_connections

    sockets = []

    # -------------------------------------------------------------
    # Connection establishment
    # -------------------------------------------------------------

    for client_id in client_ids:

        if stop_event.is_set():
            break

        try:

            sock = socket.socket(
                socket.AF_INET,
                socket.SOCK_STREAM,
            )

            sock.settimeout(5)

            sock.connect((HOST, PORT))

            establish(sock)

            # No timeout during throughput phase.
            sock.settimeout(None)

            sockets.append(sock)

            with stats_lock:
                active_connections += 1

        except Exception as e:

            with stats_lock:
                failed_connections += 1

            print(
                f"[CONNECT FAIL] "
                f"worker={worker_id:02d} "
                f"client={client_id} "
                f"error={e}",
                flush=True,
            )

    print(
        f"[WORKER {worker_id:02d}] "
        f"established={len(sockets)}",
        flush=True,
    )

    ready_event.set()

    # -------------------------------------------------------------
    # Wait until ALL workers have established their connections.
    # -------------------------------------------------------------

    start_event.wait()

    if stop_event.is_set():

        for sock in sockets:
            try:
                sock.close()
            except Exception:
                pass

        return

    # -------------------------------------------------------------
    # Maximum-rate send loop
    # -------------------------------------------------------------

    local_messages = 0
    local_bytes = 0
    local_failures = 0

    last_flush = time.monotonic()

    while not stop_event.is_set():

        # Send complete packets as quickly as possible.
        for sock in sockets:

            if stop_event.is_set():
                break

            try:

                sock.sendall(PUBLISH_PACKET)

                local_messages += 1
                local_bytes += PACKET_SIZE

            except Exception:

                local_failures += 1

        # ---------------------------------------------------------
        # Flush statistics periodically.
        #
        # 100ms gives us useful live telemetry without putting
        # a global lock on every packet.
        # ---------------------------------------------------------

        now = time.monotonic()

        if now - last_flush >= 0.1:

            flush_stats(
                local_messages,
                local_bytes,
                local_failures,
            )

            local_messages = 0
            local_bytes = 0
            local_failures = 0

            last_flush = now

    # Flush anything remaining.
    flush_stats(
        local_messages,
        local_bytes,
        local_failures,
    )

    # -------------------------------------------------------------
    # Cleanup
    # -------------------------------------------------------------

    for sock in sockets:

        try:
            sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------

print()
print("=" * 72)
print(" VELORA MAXIMUM THROUGHPUT TEST")
print("=" * 72)

print()
print(f"[CONFIG] host              : {HOST}")
print(f"[CONFIG] port              : {PORT}")
print(f"[CONFIG] connections       : {CLIENTS}")
print(f"[CONFIG] payload           : {PAYLOAD_SIZE} bytes")
print(f"[CONFIG] packet            : {PACKET_SIZE} bytes")
print(f"[CONFIG] workers           : {THREADS}")
print(f"[CONFIG] duration          : {DURATION:.1f} seconds")

print()
print("[MODE]")
print("  Complete valid PUBLISH packets")
print("  No artificial delay")
print("  Persistent TCP connections")
print("  Maximum client-side send rate")
print("  Throughput measured AFTER connection setup")
print()


# ---------------------------------------------------------------------
# Divide connections between workers
# ---------------------------------------------------------------------

worker_clients = [
    []
    for _ in range(THREADS)
]

for client_id in range(CLIENTS):
    worker_clients[client_id % THREADS].append(client_id)


# ---------------------------------------------------------------------
# Start workers
# ---------------------------------------------------------------------

threads = []
ready_events = []

start_event = threading.Event()
stop_event = threading.Event()

for worker_id in range(THREADS):

    if not worker_clients[worker_id]:
        continue

    ready_event = threading.Event()
    ready_events.append(ready_event)

    thread = threading.Thread(
        target=worker,
        args=(
            worker_id,
            worker_clients[worker_id],
            ready_event,
            start_event,
            stop_event,
        ),
        daemon=True,
    )

    threads.append(thread)
    thread.start()


# ---------------------------------------------------------------------
# Wait for every worker to finish connection establishment
# ---------------------------------------------------------------------

print()
print("[SETUP] Waiting for all workers to establish connections...")

for event in ready_events:
    event.wait()

messages, bytes_sent, failures, active, failed_conn = get_stats()

print()
print("[SETUP COMPLETE]")
print(f"  Requested connections : {CLIENTS}")
print(f"  Active connections    : {active}")
print(f"  Failed connections    : {failed_conn}")

if active == 0:

    print()
    print("[FATAL] No connections established.")
    stop_event.set()

    for thread in threads:
        thread.join()

    sys.exit(1)

if active < CLIENTS:

    print()
    print(
        f"[WARNING] Only {active}/{CLIENTS} "
        f"connections established."
    )


# ---------------------------------------------------------------------
# START BENCHMARK
# ---------------------------------------------------------------------

print()
print("[BENCHMARK] Starting throughput measurement...")
print("[BENCHMARK] All workers released.")
print()

benchmark_start = time.monotonic()

start_event.set()

last_time = benchmark_start
last_messages = 0
last_bytes = 0
last_failures = 0

# ---------------------------------------------------------------------
# Live monitoring
# ---------------------------------------------------------------------

try:

    while True:

        time.sleep(1.0)

        now = time.monotonic()
        elapsed = now - benchmark_start

        messages, bytes_sent, failures, active, failed_conn = get_stats()

        interval = now - last_time

        interval_messages = messages - last_messages
        interval_bytes = bytes_sent - last_bytes
        interval_failures = failures - last_failures

        msg_rate = (
            interval_messages / interval
            if interval > 0
            else 0
        )

        byte_rate = (
            interval_bytes / interval
            if interval > 0
            else 0
        )

        print(
            f"[{elapsed:7.2f}s] "
            f"active={active:5d} "
            f"rate={msg_rate:10,.0f} msg/s "
            f"network={byte_rate / 1024 / 1024:8.2f} MiB/s "
            f"total={messages:12,} "
            f"failures={failures:,}",
            flush=True,
        )

        last_time = now
        last_messages = messages
        last_bytes = bytes_sent
        last_failures = failures

        if elapsed >= DURATION:

            break

except KeyboardInterrupt:

    print()
    print("[STOP] Interrupt received.")


# ---------------------------------------------------------------------
# Stop workers
# ---------------------------------------------------------------------

stop_event.set()

for thread in threads:
    thread.join()


# ---------------------------------------------------------------------
# Final statistics
# ---------------------------------------------------------------------

benchmark_end = time.monotonic()
elapsed = benchmark_end - benchmark_start

messages, bytes_sent, failures, active, failed_conn = get_stats()

avg_msg_rate = (
    messages / elapsed
    if elapsed > 0
    else 0
)

avg_byte_rate = (
    bytes_sent / elapsed
    if elapsed > 0
    else 0
)


print()
print("=" * 72)
print(" FINAL RESULTS")
print("=" * 72)

print()
print(f"  Benchmark runtime       : {elapsed:.2f} s")
print(f"  Requested connections   : {CLIENTS}")
print(f"  Active connections      : {active}")
print(f"  Failed connections      : {failed_conn}")
print()
print(f"  Packet size             : {PACKET_SIZE} bytes")
print(f"  Payload size            : {PAYLOAD_SIZE} bytes")
print()
print(f"  Messages sent           : {messages:,}")
print(f"  Bytes sent              : {bytes_sent:,}")
print(f"  Send failures           : {failures:,}")
print()
print(
    f"  Average throughput      : "
    f"{avg_msg_rate:,.0f} messages/sec"
)
print(
    f"  Network throughput      : "
    f"{avg_byte_rate / 1024 / 1024:,.2f} MiB/sec"
)

if elapsed > 0:

    print(
        f"  Average packets/conn   : "
        f"{messages / max(active, 1):,.0f}"
    )

print()

if failures == 0:

    print("[RESULT] PASS - no send failures")

else:

    print(
        f"[RESULT] WARNING - "
        f"{failures:,} send failures"
    )

print("=" * 72)
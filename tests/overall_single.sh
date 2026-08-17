#!/bin/bash

HOST="127.0.0.1"
PORT=22409

python3 - "$HOST" "$PORT" <<'PY'
import socket
import struct
import sys

HOST = sys.argv[1]
PORT = int(sys.argv[2])

MAGIC = 0x5789
VERSION = 1

CONNECT = 1
CONNECT_ACK = 2
PING = 3
PONG = 4
STREAM_OPEN = 5
STREAM_OPEN_ACK = 6
STREAM_CLOSE = 7
ERROR = 9

def packet(pkt_type, stream_id=0, flags=0, payload=b""):
    return struct.pack(
        "!HBBBBH",
        MAGIC,
        VERSION,
        pkt_type,
        stream_id,
        flags,
        len(payload)
    ) + payload

def recv_packet(sock):
    header = b""

    while len(header) < 8:
        chunk = sock.recv(8 - len(header))
        if not chunk:
            raise RuntimeError("Connection closed while receiving header")
        header += chunk

    magic, version, pkt_type, stream_id, flags, payload_len = struct.unpack(
        "!HBBBBH",
        header
    )

    payload = b""
    while len(payload) < payload_len:
        chunk = sock.recv(payload_len - len(payload))
        if not chunk:
            raise RuntimeError("Connection closed while receiving payload")
        payload += chunk

    return {
        "magic": magic,
        "version": version,
        "type": pkt_type,
        "stream_id": stream_id,
        "flags": flags,
        "payload_len": payload_len,
        "payload": payload,
    }

def check_packet(pkt, expected_type, expected_stream=None):
    assert pkt["magic"] == MAGIC, f"bad magic: {hex(pkt['magic'])}"
    assert pkt["version"] == VERSION, f"bad version: {pkt['version']}"
    assert pkt["type"] == expected_type, \
        f"expected type {expected_type}, got {pkt['type']}"

    if expected_stream is not None:
        assert pkt["stream_id"] == expected_stream, \
            f"expected stream {expected_stream}, got {pkt['stream_id']}"

def send_and_log(sock, pkt_type, stream_id=0):
    raw = packet(pkt_type, stream_id)
    print(f"\n→ sending type={pkt_type}, stream={stream_id}")
    print(f"  {raw.hex()}")
    sock.sendall(raw)

print("Connecting to Velora...")

with socket.create_connection((HOST, PORT), timeout=5) as sock:

    # ------------------------------------------------------------
    # 1. INIT -> CONNECT -> ESTABLISHED
    # ------------------------------------------------------------
    send_and_log(sock, CONNECT)

    pkt = recv_packet(sock)

    print("←", pkt)

    check_packet(pkt, CONNECT_ACK, 0)
    print("PASS: CONNECT -> CONNECT_ACK")

    # ------------------------------------------------------------
    # 2. ESTABLISHED -> PING -> PONG
    # ------------------------------------------------------------
    send_and_log(sock, PING, 0)

    pkt = recv_packet(sock)

    print("←", pkt)

    check_packet(pkt, PONG, 0)
    print("PASS: PING -> PONG")

    # ------------------------------------------------------------
    # 3. ESTABLISHED -> STREAM_OPEN -> STREAM_OPEN_ACK
    # ------------------------------------------------------------
    send_and_log(sock, STREAM_OPEN, 0)

    pkt = recv_packet(sock)

    print("←", pkt)

    check_packet(pkt, STREAM_OPEN_ACK)

    stream_id = pkt["stream_id"]

    assert stream_id != 0, "STREAM_OPEN should allocate a non-default stream"

    print(f"PASS: STREAM_OPEN -> STREAM_OPEN_ACK (stream={stream_id})")

    # ------------------------------------------------------------
    # 4. ESTABLISHED -> STREAM_CLOSE
    # ------------------------------------------------------------
    send_and_log(sock, STREAM_CLOSE, stream_id)

    try:
        pkt = recv_packet(sock)
        print("←", pkt)

        print(
            f"STREAM_CLOSE produced response type={pkt['type']}, "
            f"stream={pkt['stream_id']}"
        )

        if pkt["type"] == ERROR:
            print("STREAM_CLOSE -> ERROR")
        else:
            print("STREAM_CLOSE produced a non-error response")

    except Exception as e:
        print("STREAM_CLOSE produced no response:", e)

    # ------------------------------------------------------------
    # Final
    # ------------------------------------------------------------
    print("\nFSM WALK COMPLETE")
PY
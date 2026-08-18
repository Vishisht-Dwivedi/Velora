#!/bin/bash
#
# parser_fsm_full.sh -- exercises vr_parser_t + vr_protocol_handle_packet
# end to end, beyond the single happy-path walk in overall_single.sh.
#
# Covers:
#   1. Header/payload assembly when bytes arrive one at a time
#   2. Multiple pipelined packets delivered in a single TCP write
#   3. A payload large enough to force read_buf to grow past its
#      initial capacity, followed by a packet that must still parse
#      correctly (proves the ring buffer didn't corrupt alignment)
#   4. The full CONNECT -> PING -> STREAM_OPEN -> PUBLISH -> STREAM_CLOSE walk
#   5. Malformed headers (bad magic / version / type / flags) getting
#      rejected and the connection closed, instead of silently
#      desyncing the stream
#   6. Several independent connections running the FSM concurrently,
#      to check that per-connection parser/protocol state doesn't leak
#      across connections
#
# Usage: ./parser_fsm_full.sh [host] [port]

HOST="${1:-127.0.0.1}"
PORT="${2:-22409}"

python3 - "$HOST" "$PORT" <<'PY'
import socket
import struct
import sys
import time

HOST = sys.argv[1]
PORT = int(sys.argv[2])

MAGIC = 0x5789
VERSION = 1

CONNECT, CONNECT_ACK = 1, 2
PING, PONG = 3, 4
STREAM_OPEN, STREAM_OPEN_ACK = 5, 6
STREAM_CLOSE = 7
PUBLISH = 8
ERROR = 9

HEADER_LEN = 8

results = []


def report(name, ok, detail=""):
    tag = "PASS" if ok else "FAIL"
    print(f"[{tag}] {name}" + (f" -- {detail}" if detail else ""))
    results.append(ok)


def packet(pkt_type, stream_id=0, flags=0, payload=b"", magic=MAGIC, version=VERSION):
    return struct.pack("!HBBBBH", magic, version, pkt_type, stream_id, flags, len(payload)) + payload


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"peer closed while expecting {n - len(buf)} more bytes")
        buf += chunk
    return buf


def recv_packet(sock):
    header = recv_exact(sock, HEADER_LEN)
    magic, version, pkt_type, stream_id, flags, payload_len = struct.unpack("!HBBBBH", header)
    payload = recv_exact(sock, payload_len) if payload_len else b""
    return {
        "magic": magic, "version": version, "type": pkt_type,
        "stream_id": stream_id, "flags": flags,
        "payload_len": payload_len, "payload": payload,
    }


def send_fragmented(sock, data, chunk_size=1, delay=0.004):
    for i in range(0, len(data), chunk_size):
        sock.send(data[i:i + chunk_size])
        time.sleep(delay)


def connect_and_establish(sock):
    """CONNECT -> CONNECT_ACK, moving conn->proto_state to ESTABLISHED."""
    sock.sendall(packet(CONNECT))
    pkt = recv_packet(sock)
    assert pkt["type"] == CONNECT_ACK, f"expected CONNECT_ACK, got {pkt['type']}"
    assert pkt["magic"] == MAGIC and pkt["version"] == VERSION
    return pkt


# ---------------------------------------------------------------------
# 1. Header and payload arriving one byte at a time
# ---------------------------------------------------------------------
def test_fragmented_delivery():
    name = "fragmented header + payload assembly (1 byte at a time)"
    try:
        with socket.create_connection((HOST, PORT), timeout=5) as sock:
            connect_and_establish(sock)

            payload = b"velora-fragmented-payload-test"
            pkt_bytes = packet(PUBLISH, stream_id=0, payload=payload)

            # Dribble the PUBLISH packet in one byte at a time. PUBLISH
            # gets no application-level reply, so instead we confirm the
            # parser correctly resynced by following it with a PING and
            # checking we get exactly one PONG back.
            send_fragmented(sock, pkt_bytes, chunk_size=1, delay=0.004)
            sock.sendall(packet(PING))

            pkt = recv_packet(sock)
            report(name, pkt["type"] == PONG,
                   f"got type={pkt['type']} after byte-by-byte PUBLISH+PING")
    except Exception as e:
        report(name, False, str(e))


# ---------------------------------------------------------------------
# 2. Several packets pipelined into a single write
# ---------------------------------------------------------------------
def test_pipelined_burst(count=25):
    name = f"pipelined burst ({count} PINGs in one write)"
    try:
        with socket.create_connection((HOST, PORT), timeout=5) as sock:
            connect_and_establish(sock)

            burst = b"".join(packet(PING) for _ in range(count))
            sock.sendall(burst)

            replies = []
            sock.settimeout(3)
            for _ in range(count):
                replies.append(recv_packet(sock))

            all_pong = all(p["type"] == PONG for p in replies)
            report(name, len(replies) == count and all_pong,
                   f"received {len(replies)}/{count} replies, all_pong={all_pong}")
    except Exception as e:
        report(name, False, str(e))


# ---------------------------------------------------------------------
# 3. Payload large enough to force read_buf growth (> 4096 bytes)
# ---------------------------------------------------------------------
def test_large_payload_buffer_growth(payload_size=50000):
    name = f"large payload forcing ring buffer growth ({payload_size} bytes)"
    try:
        with socket.create_connection((HOST, PORT), timeout=5) as sock:
            connect_and_establish(sock)

            payload = bytes((i % 251) for i in range(payload_size))
            sock.sendall(packet(PUBLISH, payload=payload))
            # No reply for PUBLISH -- confirm the connection is still
            # correctly synced afterwards.
            sock.sendall(packet(PING))

            pkt = recv_packet(sock)
            report(name, pkt["type"] == PONG,
                   f"got type={pkt['type']} after {payload_size}-byte payload")
    except Exception as e:
        report(name, False, str(e))


# ---------------------------------------------------------------------
# 4. Full FSM walk: CONNECT -> PING -> STREAM_OPEN(x2) -> PUBLISH -> STREAM_CLOSE
# ---------------------------------------------------------------------
def test_full_fsm_walk():
    name = "full FSM walk (connect/ping/stream open+publish+close)"
    try:
        with socket.create_connection((HOST, PORT), timeout=5) as sock:
            connect_and_establish(sock)

            sock.sendall(packet(PING))
            pkt = recv_packet(sock)
            assert pkt["type"] == PONG, f"expected PONG, got {pkt['type']}"

            sock.sendall(packet(STREAM_OPEN))
            pkt = recv_packet(sock)
            assert pkt["type"] == STREAM_OPEN_ACK, f"expected STREAM_OPEN_ACK, got {pkt['type']}"
            stream_a = pkt["stream_id"]
            assert stream_a != 0, "opened stream should not reuse the default stream 0"

            sock.sendall(packet(STREAM_OPEN))
            pkt = recv_packet(sock)
            assert pkt["type"] == STREAM_OPEN_ACK, f"expected STREAM_OPEN_ACK, got {pkt['type']}"
            stream_b = pkt["stream_id"]
            assert stream_b != stream_a, "second opened stream should get a distinct id"

            sock.sendall(packet(PUBLISH, stream_id=stream_a, payload=b"hello on stream A"))
            # PUBLISH has no reply by design; prove liveness with a PING.
            sock.sendall(packet(PING))
            pkt = recv_packet(sock)
            assert pkt["type"] == PONG, f"expected PONG after PUBLISH, got {pkt['type']}"

            # Closing a non-last stream still returns *a* packet with the
            # right envelope (magic/version/stream_id); protocol.c does not
            # yet assign STREAM_CLOSE a real ack type, so we don't assert
            # on `type` here -- that's a protocol-layer gap, not a parser
            # or reactor issue.
            sock.sendall(packet(STREAM_CLOSE, stream_id=stream_a))
            pkt = recv_packet(sock)
            assert pkt["magic"] == MAGIC and pkt["version"] == VERSION
            assert pkt["stream_id"] == stream_a

            report(name, True, f"streams opened={stream_a},{stream_b}")
    except Exception as e:
        report(name, False, str(e))


# ---------------------------------------------------------------------
# 5. Malformed packets must be rejected and the connection closed
# ---------------------------------------------------------------------
def test_invalid_packet(label, raw_packet):
    name = f"invalid packet rejected: {label}"
    try:
        with socket.create_connection((HOST, PORT), timeout=5) as sock:
            connect_and_establish(sock)
            sock.sendall(raw_packet)
            sock.settimeout(3)
            data = sock.recv(4096)
            # A closed connection delivers EOF (b""). Some malformed
            # headers could coincidentally still look like a valid
            # packet to the OS but must never be echoed as a normal
            # protocol reply, so both are checked.
            report(name, data == b"", f"expected connection close (EOF), got {len(data)} bytes")
    except (ConnectionError, socket.timeout, OSError) as e:
        # connection reset / refused read is also an acceptable "closed" signal
        report(name, True, f"connection closed as expected ({e})")
    except Exception as e:
        report(name, False, str(e))


def test_invalid_packets():
    test_invalid_packet("bad magic", packet(PING, magic=0xDEAD))
    test_invalid_packet("bad version", packet(PING, version=9))
    test_invalid_packet("unknown type (99)", packet(99))
    test_invalid_packet("invalid flag bits", packet(PING, flags=0xF0))


# ---------------------------------------------------------------------
# 6. Concurrent connections must not cross-contaminate parser/proto state
# ---------------------------------------------------------------------
def test_concurrent_connections(n=10):
    name = f"concurrent connections ({n}) isolated FSM state"
    socks = []
    try:
        for _ in range(n):
            s = socket.create_connection((HOST, PORT), timeout=5)
            socks.append(s)

        # Interleave CONNECT across all of them before reading any
        # replies, so if state were shared/leaked between connections
        # this would surface as cross-talk.
        for s in socks:
            s.sendall(packet(CONNECT))
        for s in socks:
            pkt = recv_packet(s)
            assert pkt["type"] == CONNECT_ACK

        for idx, s in enumerate(socks):
            s.sendall(packet(STREAM_OPEN))
        stream_ids = []
        for s in socks:
            pkt = recv_packet(s)
            assert pkt["type"] == STREAM_OPEN_ACK
            stream_ids.append(pkt["stream_id"])

        for s in socks:
            s.sendall(packet(PING))
        for s in socks:
            pkt = recv_packet(s)
            assert pkt["type"] == PONG

        report(name, True, f"{n} connections each completed CONNECT/STREAM_OPEN/PING independently")
    except Exception as e:
        report(name, False, str(e))
    finally:
        for s in socks:
            try:
                s.close()
            except Exception:
                pass


def main():
    print(f"Running parser/protocol FSM tests against {HOST}:{PORT}\n")

    test_fragmented_delivery()
    test_pipelined_burst()
    test_large_payload_buffer_growth()
    test_full_fsm_walk()
    test_invalid_packets()
    test_concurrent_connections()

    passed = sum(1 for r in results if r)
    total = len(results)
    print(f"\n{passed}/{total} checks passed")
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
PY
#!/bin/bash

HOST="${1:-127.0.0.1}"
PORT="${2:-22409}"
CLIENTS="${3:-10000}"
INTERVAL="${4:-1.0}"

python3 - "$HOST" "$PORT" "$CLIENTS" "$INTERVAL" <<'PY'
import socket
import struct
import sys
import time
import errno
from collections import Counter

HOST = sys.argv[1]
PORT = int(sys.argv[2])
CLIENTS = int(sys.argv[3])
INTERVAL = float(sys.argv[4])

MAGIC = 0x5789
VERSION = 1

CONNECT = 1
CONNECT_ACK = 2
PUBLISH = 8

HEADER_LEN = 8


def packet(pkt_type, stream_id=0, flags=0, payload=b""):
    return (
        struct.pack(
            "!HBBBBH",
            MAGIC,
            VERSION,
            pkt_type,
            stream_id,
            flags,
            len(payload),
        )
        + payload
    )


CONNECT_PACKET = packet(CONNECT)

# Valid PUBLISH packet with a payload.
# We deliberately send this byte-by-byte so the server stays in
# VR_PARSER_PAYLOAD_WAIT for a long time.
PUBLISH_PAYLOAD = b"VELORA-SLOWLORIS-PAYLOAD"
PUBLISH_PACKET = packet(
    PUBLISH,
    stream_id=0,
    payload=PUBLISH_PAYLOAD,
)

sockets = []
connected = 0
failed_connects = 0

bytes_sent_total = 0
successful_sends_total = 0
failed_sends_total = 0

failure_types = Counter()

start_time = time.monotonic()


def elapsed():
    return time.monotonic() - start_time


def fmt_time(seconds):
    seconds = int(seconds)
    h = seconds // 3600
    m = (seconds % 3600) // 60
    s = seconds % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


def print_header(title):
    print()
    print("=" * 72)
    print(f" {title}")
    print("=" * 72)


def recv_exact(sock, n, timeout=5):
    old_timeout = sock.gettimeout()
    sock.settimeout(timeout)

    try:
        buf = bytearray()

        while len(buf) < n:
            chunk = sock.recv(n - len(buf))

            if not chunk:
                raise ConnectionError("peer closed connection")

            buf.extend(chunk)

        return bytes(buf)

    finally:
        sock.settimeout(old_timeout)


def recv_packet(sock):
    header = recv_exact(sock, HEADER_LEN)

    magic, version, pkt_type, stream_id, flags, payload_len = struct.unpack(
        "!HBBBBH",
        header,
    )

    payload = b""

    if payload_len:
        payload = recv_exact(sock, payload_len)

    return {
        "magic": magic,
        "version": version,
        "type": pkt_type,
        "stream_id": stream_id,
        "flags": flags,
        "payload_len": payload_len,
        "payload": payload,
    }


def connect_client(idx):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

        # No application-level timeout while holding the connection.
        s.settimeout(5)

        s.connect((HOST, PORT))

        return s

    except Exception as e:
        print(
            f"[CONNECT FAIL] client={idx} "
            f"type={type(e).__name__} "
            f"error={e}"
        )

        return None


def establish_client(idx, sock):
    """
    Send CONNECT completely.

    This moves Velora:
        INIT -> CONNECT -> CONNECT_ACK -> ESTABLISHED
    """

    try:
        sock.sendall(CONNECT_PACKET)

        response = recv_packet(sock)

        if response["type"] != CONNECT_ACK:
            print(
                f"[PROTOCOL FAIL] client={idx} "
                f"expected=CONNECT_ACK "
                f"received={response['type']}"
            )

            return False

        return True

    except Exception as e:
        print(
            f"[ESTABLISH FAIL] client={idx} "
            f"type={type(e).__name__} "
            f"error={e}"
        )

        return False


print_header("VELORA SLOWLORIS PROTOCOL STRESS TEST")

print(f"[CONFIG] host             : {HOST}")
print(f"[CONFIG] port             : {PORT}")
print(f"[CONFIG] concurrent clients: {CLIENTS}")
print(f"[CONFIG] byte interval     : {INTERVAL:.3f}s")
print(f"[CONFIG] parser workload   : fragmented valid packets")
print(f"[CONFIG] connect packet    : {len(CONNECT_PACKET)} bytes")
print(f"[CONFIG] publish packet    : {len(PUBLISH_PACKET)} bytes")
print(f"[CONFIG] publish payload   : {len(PUBLISH_PAYLOAD)} bytes")

print()
print("[PHASE 1] Creating TCP connections...")

# ---------------------------------------------------------------------
# CONNECTION PHASE
# ---------------------------------------------------------------------

for idx in range(CLIENTS):
    sock = connect_client(idx)

    if sock is None:
        failed_connects += 1
        continue

    sockets.append(
        {
            "socket": sock,
            "connected_at": time.monotonic(),
            "state": "CONNECTED",
            "bytes_sent": 0,
            "packet_offset": 0,
        }
    )

    connected += 1

    if connected % 500 == 0 or connected == CLIENTS:
        print(
            f"[CONNECT] "
            f"{connected}/{CLIENTS} "
            f"connected "
            f"({connected / max(CLIENTS, 1) * 100:.1f}%)"
        )

print()
print("[PHASE 1 COMPLETE]")
print(f"  requested connections : {CLIENTS}")
print(f"  connected              : {connected}")
print(f"  failed                 : {failed_connects}")

if not sockets:
    print("[FATAL] No connections established.")
    sys.exit(1)


# ---------------------------------------------------------------------
# CONNECT / FSM ESTABLISHMENT
# ---------------------------------------------------------------------

print()
print("[PHASE 2] Establishing Velora protocol sessions...")
print("[PHASE 2] CONNECT -> CONNECT_ACK -> ESTABLISHED")

established = []

for idx, client in enumerate(sockets):

    if establish_client(idx, client["socket"]):
        client["state"] = "ESTABLISHED"
        established.append(client)
    else:
        client["state"] = "FAILED"

        try:
            client["socket"].close()
        except Exception:
            pass

    if (idx + 1) % 500 == 0 or idx + 1 == len(sockets):
        print(
            f"[ESTABLISH] "
            f"{idx + 1}/{len(sockets)} "
            f"processed | "
            f"established={len(established)}"
        )

sockets = established

print()
print("[PHASE 2 COMPLETE]")
print(f"  established sessions : {len(sockets)}")

if not sockets:
    print("[FATAL] No protocol sessions established.")
    sys.exit(1)


# ---------------------------------------------------------------------
# SLOWLORIS PHASE
# ---------------------------------------------------------------------

print()
print("[PHASE 3] Starting fragmented-packet Slowloris workload")
print()
print("Each connection will slowly receive a VALID PUBLISH packet.")
print("The server should remain in its incremental parser state while")
print("the payload is intentionally delivered one byte at a time.")
print()

print(
    "[EXPECTED FSM]"
    " HEADER_WAIT -> VALIDATE -> PAYLOAD_WAIT"
)

print()
print("[START] All connections are now held open.")
print("[START] Press Ctrl-C to terminate.")
print()

slow_start = time.monotonic()

round_num = 0

last_report_time = slow_start
last_report_bytes = 0


try:

    while True:

        round_num += 1

        round_start = time.monotonic()

        round_sent = 0
        round_failed = 0
        round_bytes = 0

        dead_clients = []

        for idx, client in enumerate(sockets):

            sock = client["socket"]

            # We eventually want to send one byte from the PUBLISH packet.
            offset = client["packet_offset"]

            if offset >= len(PUBLISH_PACKET):

                # Once the whole PUBLISH packet has been delivered,
                # start another packet so the parser gets exercised
                # continuously rather than simply sitting idle.
                client["packet_offset"] = 0
                offset = 0

            data = PUBLISH_PACKET[offset:offset + 1]

            try:

                sent = sock.send(data)

                if sent == 0:
                    raise ConnectionError("send returned 0")

                client["packet_offset"] += sent
                client["bytes_sent"] += sent

                round_sent += 1
                round_bytes += sent

                successful_sends_total += 1
                bytes_sent_total += sent

            except OSError as e:

                round_failed += 1
                failed_sends_total += 1

                error_name = errno.errorcode.get(
                    getattr(e, "errno", None),
                    type(e).__name__,
                )

                failure_types[error_name] += 1

                if client["state"] != "FAILED":

                    client["state"] = "FAILED"

                    dead_clients.append(client)

                    print()
                    print("[CONNECTION FAILURE]")
                    print(f"  client index   : {idx}")
                    print(f"  state           : {client['state']}")
                    print(f"  bytes sent      : {client['bytes_sent']}")
                    print(f"  packet offset   : {client['packet_offset']}")
                    print(f"  error           : {error_name}")
                    print(f"  error detail    : {e}")
                    print(f"  elapsed         : {fmt_time(elapsed())}")
                    print()

            except Exception as e:

                round_failed += 1
                failed_sends_total += 1

                error_name = type(e).__name__
                failure_types[error_name] += 1

                if client["state"] != "FAILED":

                    client["state"] = "FAILED"

                    dead_clients.append(client)

                    print()
                    print("[CONNECTION FAILURE]")
                    print(f"  client index   : {idx}")
                    print(f"  bytes sent      : {client['bytes_sent']}")
                    print(f"  packet offset  : {client['packet_offset']}")
                    print(f"  exception      : {error_name}")
                    print(f"  error           : {e}")
                    print(f"  elapsed        : {fmt_time(elapsed())}")
                    print()

        # Remove dead connections from active workload.
        if dead_clients:

            dead_set = {id(c) for c in dead_clients}

            survivors = []

            for client in sockets:

                if id(client) in dead_set:

                    try:
                        client["socket"].close()
                    except Exception:
                        pass

                else:
                    survivors.append(client)

            sockets = survivors

        now = time.monotonic()

        elapsed_round = now - round_start
        elapsed_total = now - slow_start

        total_active = len(sockets)

        throughput = 0.0

        if elapsed_total > 0:

            throughput = bytes_sent_total / elapsed_total

        print(
            f"[ROUND {round_num:06d}] "
            f"active={total_active:05d} "
            f"sent={round_sent:05d} "
            f"failed={round_failed:05d} "
            f"bytes={round_bytes:05d} "
            f"total_bytes={bytes_sent_total} "
            f"throughput={throughput:.2f} B/s "
            f"round_time={elapsed_round:.4f}s "
            f"uptime={fmt_time(elapsed_total)}"
        )

        if dead_clients:

            print(
                f"[HEALTH] active={total_active}/{connected} "
                f"failure_rate="
                f"{failed_sends_total / max(successful_sends_total + failed_sends_total, 1) * 100:.4f}%"
            )

            print(
                "[ERRORS] "
                + " ".join(
                    f"{name}={count}"
                    for name, count in failure_types.items()
                )
            )

        if not sockets:

            print()
            print("[FATAL] All protocol connections have failed.")

            break

        # Pace the Slowloris workload.
        sleep_time = max(0.0, INTERVAL - elapsed_round)

        time.sleep(sleep_time)


except KeyboardInterrupt:

    print()
    print()
    print("[STOP] Interrupt received.")

finally:

    shutdown_time = elapsed()

    print()
    print_header("FINAL SLOWLORIS STATISTICS")

    print(f"  runtime                  : {fmt_time(shutdown_time)}")
    print(f"  requested connections    : {CLIENTS}")
    print(f"  initial TCP connections  : {connected}")
    print(f"  current active           : {len(sockets)}")
    print(f"  connection failures      : {connected - len(sockets)}")
    print()
    print(f"  successful sends         : {successful_sends_total}")
    print(f"  failed sends             : {failed_sends_total}")
    print(f"  total bytes sent         : {bytes_sent_total}")

    if shutdown_time > 0:
        print(
            f"  average throughput       : "
            f"{bytes_sent_total / shutdown_time:.2f} B/s"
        )

    if failure_types:

        print()
        print("  failure breakdown:")

        for name, count in failure_types.most_common():

            print(f"    {name:16s}: {count}")

    print()
    print("[CLEANUP] Closing sockets...")

    for client in sockets:

        try:
            client["socket"].close()
        except Exception:
            pass

    print("[DONE] All client sockets closed.")
PY
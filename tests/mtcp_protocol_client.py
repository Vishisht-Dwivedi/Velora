#!/usr/bin/env python3
"""Validate Velora CONNECT and PING responses over a TCP connection."""

import argparse
import socket
import struct
import sys


MAGIC = 0x5789
VERSION = 1
HEADER = struct.Struct("!HBBBBH")
CONNECT = 1
CONNECT_ACK = 2
PING = 3
PONG = 4


class ProtocolTestError(Exception):
    pass


def encode_packet(packet_type):
    return HEADER.pack(MAGIC, VERSION, packet_type, 0, 0, 0)


def receive_exact(sock, length):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise ProtocolTestError(
                f"unexpected EOF after {len(data)} of {length} expected bytes"
            )
        data.extend(chunk)
    return bytes(data)


def receive_response(sock, expected_type):
    raw_header = receive_exact(sock, HEADER.size)
    magic, version, packet_type, stream_id, flags, payload_length = HEADER.unpack(
        raw_header
    )
    if magic != MAGIC:
        raise ProtocolTestError(f"invalid response magic: 0x{magic:04x}")
    if version != VERSION:
        raise ProtocolTestError(f"invalid response version: {version}")
    if packet_type != expected_type:
        raise ProtocolTestError(
            f"unexpected response type {packet_type}; expected {expected_type}"
        )
    if stream_id != 0:
        raise ProtocolTestError(f"unexpected response stream: {stream_id}")
    if flags != 0:
        raise ProtocolTestError(f"unexpected response flags: {flags}")
    if payload_length != 0:
        raise ProtocolTestError(
            f"unexpected response payload length: {payload_length}"
        )


def run_test(host, port, timeout):
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(encode_packet(CONNECT))
        receive_response(sock, CONNECT_ACK)
        print("PASS CONNECT_ACK")

        sock.sendall(encode_packet(PING))
        receive_response(sock, PONG)
        print("PASS PING/PONG")

        pipeline_count = 5
        sock.sendall(encode_packet(PING) * pipeline_count)
        for _ in range(pipeline_count):
            receive_response(sock, PONG)
        print(f"PASS pipelined PING/PONG ({pipeline_count})")

        fragmented_ping = encode_packet(PING)
        sock.sendall(fragmented_ping[:3])
        sock.sendall(fragmented_ping[3:])
        receive_response(sock, PONG)
        print("PASS fragmented-header PING/PONG")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="server IPv4 address or hostname")
    parser.add_argument("port", type=int, help="server TCP port")
    parser.add_argument("--timeout", type=float, default=3.0, help="socket timeout")
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    if args.timeout <= 0:
        parser.error("timeout must be greater than zero")

    try:
        run_test(args.host, args.port, args.timeout)
    except (OSError, ProtocolTestError) as exc:
        print(f"FAIL {exc}", file=sys.stderr)
        return 1
    print("PASS all protocol checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
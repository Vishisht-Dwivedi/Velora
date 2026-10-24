#!/usr/bin/env python3
"""Capture ARP and TCP packets for one AF_XDP smoke attempt as a pcap file."""

import argparse
import signal
import socket
import struct
import time


running = True


def stop_capture(_signum, _frame):
    global running
    running = False


def wanted_packet(packet, port):
    if len(packet) < 14:
        return False

    ether_type = struct.unpack_from("!H", packet, 12)[0]
    if ether_type == 0x0806:
        return True
    if ether_type != 0x0800 or len(packet) < 34:
        return False

    ip_header_length = (packet[14] & 0x0F) * 4
    if packet[23] != socket.IPPROTO_TCP or len(packet) < 14 + ip_header_length + 4:
        return False

    source_port, destination_port = struct.unpack_from("!HH", packet, 14 + ip_header_length)
    return source_port == port or destination_port == port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("interface")
    parser.add_argument("pcap_path")
    parser.add_argument("port", type=int)
    args = parser.parse_args()

    signal.signal(signal.SIGINT, stop_capture)
    signal.signal(signal.SIGTERM, stop_capture)

    packet_socket = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    packet_socket.bind((args.interface, 0))
    packet_socket.settimeout(0.25)

    with open(args.pcap_path, "wb") as capture_file:
        capture_file.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        packet_count = 0
        started_at = time.monotonic()
        while running:
            try:
                packet, _address = packet_socket.recvfrom(65535)
            except socket.timeout:
                continue
            if not wanted_packet(packet, args.port):
                continue

            captured_at = time.time()
            seconds = int(captured_at)
            microseconds = int((captured_at - seconds) * 1_000_000)
            capture_file.write(struct.pack("<IIII", seconds, microseconds, len(packet), len(packet)))
            capture_file.write(packet)
            packet_count += 1

        capture_file.flush()
        print(f"captured {packet_count} matching packets in {time.monotonic() - started_at:.3f}s")
    packet_socket.close()


if __name__ == "__main__":
    main()
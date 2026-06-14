#!/bin/bash

HOST="127.0.0.1"
PORT=22409
CONNECTIONS=${1:-10000}

python3 << EOF
import socket
import sys
import time

HOST = "$HOST"
PORT = $PORT
CONNECTIONS = $CONNECTIONS

sockets = []

print(f"Opening {CONNECTIONS} connections...")

for i in range(CONNECTIONS):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        sockets.append(s)

        if (i + 1) % 1000 == 0:
            print(f"Opened {i + 1} connections")

    except Exception as e:
        print(f"Failed at {i}: {e}")
        break

print(f"Successfully opened {len(sockets)} connections")
print("Connections are being held open.")
print("Press Ctrl+C to close all connections.")

try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    pass

for s in sockets:
    s.close()

print("Closed all connections.")
EOF
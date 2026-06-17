#!/bin/bash

HOST="127.0.0.1"
PORT=22409
COUNT=${1:-5000}

python3 << EOF
import socket

HOST="$HOST"
PORT=$PORT
COUNT=$COUNT

sockets = []

for i in range(COUNT):
    try:
        s = socket.socket()
        s.connect((HOST, PORT))
        sockets.append(s)
    except Exception as e:
        print(f"Failed at {i}: {e}")
        break

print(f"Opened {len(sockets)}")

for s in sockets:
    s.close()

print("Closed all")
EOF
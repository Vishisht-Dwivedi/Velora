#!/bin/bash

HOST="127.0.0.1"
PORT=22409
ROUNDS=${1:-10000}

python3 << EOF
import socket

HOST="$HOST"
PORT=$PORT
ROUNDS=$ROUNDS

for i in range(ROUNDS):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        s.close()

        if (i + 1) % 1000 == 0:
            print(f"Completed {i + 1} connect/disconnect cycles")

    except Exception as e:
        print(f"Failed at {i}: {e}")
        break

print("Done")
EOF
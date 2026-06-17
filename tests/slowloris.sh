#!/bin/bash

HOST="127.0.0.1"
PORT=22409
CLIENTS=${1:-1000}

python3 << EOF
import socket
import time

HOST="$HOST"
PORT=$PORT
CLIENTS=$CLIENTS

sockets = []

for _ in range(CLIENTS):
    s = socket.socket()
    s.connect((HOST, PORT))
    sockets.append(s)

while True:
    for s in sockets:
        try:
            s.send(b"x")
        except:
            pass

    time.sleep(10)
EOF
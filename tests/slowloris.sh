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
connected = 0
failed_connects = 0

print(f"[INFO] Attempting to create {CLIENTS} clients")

for i in range(CLIENTS):
    try:
        s = socket.socket()
        s.connect((HOST, PORT))
        sockets.append(s)
        connected += 1
        if connected % 1000 == 0:
            print(f"[CONNECT] {connected}/{CLIENTS} clients connected")

    except Exception as e:
        failed_connects += 1
        print(f"[CONNECT FAIL] Client #{i}: {e}")
        print()
        print("========== CONNECTION SUMMARY ==========")
        print(f"Connected: {connected}")
        print(f"Failed:    {failed_connects}")
        print("========================================")
        print()

round_num = 0

while True:
    round_num += 1

    successful_sends = 0
    failed_sends = 0
    first_failed_client = None
    
    for idx, s in enumerate(sockets):
        try:
            s.send(b"x")
            successful_sends += 1
    
        except Exception as e:
            failed_sends += 1
    
            if first_failed_client is None:
                first_failed_client = (idx, str(e))
    
    print(
        f"[ROUND {round_num}] "
        f"sent={successful_sends} "
        f"failed={failed_sends}"
    )
    
    if first_failed_client:
        print(
            f"[FIRST FAILURE] "
            f"client={first_failed_client[0]} "
            f"error={first_failed_client[1]}"
        )
    
    time.sleep(10)
EOF

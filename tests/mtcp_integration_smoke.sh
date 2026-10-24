#!/usr/bin/env bash
set -euo pipefail

# Usage: sudo tests/mtcp_integration_smoke.sh <config-file> <port> [startup-timeout-seconds]

if [[ $# -lt 2 || $# -gt 3 ]]; then
    printf 'Usage: sudo %s <config-file> <port> [startup-timeout-seconds]\n' "$0" >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/.." && pwd -P)"
launcher="$repo_root/scripts/run-velora-mtcp.sh"
client="$repo_root/tests/mtcp_protocol_client.py"
server_namespace="velora-af-server"
client_namespace="velora-af-test1"
server_interface="vmtcp1"
client_interface="vmtcp1-peer"
server_ipv4="198.18.0.1"
client_ipv4="198.18.0.2"
client_mac="d2:ce:d7:4a:a8:37"
source_config="$1"
port="$2"
startup_timeout="${3:-15}"
keep_logs="${KEEP_LOGS:-0}"

if [[ "$EUID" -ne 0 ]]; then
    printf 'Run this integration test as root so ip netns exec can use the approved namespaces.\n' >&2
    exit 1
fi
if [[ ! "$port" =~ ^[0-9]+$ ]] || (( 10#$port < 1 || 10#$port > 65535 )); then
    printf 'Port must be an integer between 1 and 65535: %s\n' "$port" >&2
    exit 2
fi
if [[ ! "$startup_timeout" =~ ^[0-9]+$ ]] || (( 10#$startup_timeout < 1 )); then
    printf 'Startup timeout must be a positive integer: %s\n' "$startup_timeout" >&2
    exit 2
fi
if [[ "$keep_logs" != 0 && "$keep_logs" != 1 ]]; then
    printf 'KEEP_LOGS must be 0 or 1.\n' >&2
    exit 2
fi
if [[ ! -f "$source_config" ]]; then
    printf 'mTCP config file does not exist: %s\n' "$source_config" >&2
    exit 1
fi
if [[ ! -x "$launcher" ]]; then
    printf 'mTCP launcher is missing or not executable: %s\n' "$launcher" >&2
    exit 1
fi
if [[ ! -f "$client" ]]; then
    printf 'Protocol client is missing: %s\n' "$client" >&2
    exit 1
fi
for namespace in "$server_namespace" "$client_namespace"; do
    if ! ip netns list | awk '{print $1}' | grep -Fxq "$namespace"; then
        printf 'Required test namespace is missing: %s\n' "$namespace" >&2
        exit 1
    fi
done
if ! ip -n "$server_namespace" -o -4 addr show dev "$server_interface" |
    awk -v address="$server_ipv4/30" '$4 == address { found = 1 } END { exit !found }'; then
    printf 'Expected server address %s/30 is missing on %s in %s.\n' \
        "$server_ipv4" "$server_interface" "$server_namespace" >&2
    exit 1
fi
if ! ip -n "$client_namespace" -o -4 addr show dev "$client_interface" |
    awk -v address="$client_ipv4/30" '$4 == address { found = 1 } END { exit !found }'; then
    printf 'Expected client address %s/30 is missing on %s in %s.\n' \
        "$client_ipv4" "$client_interface" "$client_namespace" >&2
    exit 1
fi
if ! ip -n "$client_namespace" -o link show dev "$client_interface" |
    grep -Fqi "link/ether $client_mac"; then
    printf 'Expected client MAC %s is missing on %s in %s.\n' \
        "$client_mac" "$client_interface" "$client_namespace" >&2
    exit 1
fi
if ! ip -n "$server_namespace" -j -d link show dev "$server_interface" |
    python3 -c 'import json, sys; links = json.load(sys.stdin); sys.exit(0 if len(links) == 1 and links[0].get("num_rx_queues") == 1 else 1)'; then
    printf 'AF_XDP runtime requires exactly one RX queue on %s; refusing to launch.\n' \
        "$server_interface" >&2
    exit 1
fi
if ip -n "$server_namespace" -j -d link show dev "$server_interface" |
    python3 -c 'import json, sys; links = json.load(sys.stdin); sys.exit(0 if len(links) == 1 and "xdp" in links[0] else 1)'; then
    printf 'Refusing to launch because %s already has an XDP attachment.\n' \
        "$server_interface" >&2
    exit 1
fi

temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/velora-mtcp-smoke.XXXXXX")"
log_file="$temp_dir/server.log"
config_file="$temp_dir/afxdp_test.conf"
server_pid=""

if ! awk '
    /^[[:space:]]*port[[:space:]]*=/ {
        count++
        $0 = "port = vmtcp1"
    }
    { print }
    END { if (count != 1) exit 1 }
' "$source_config" > "$config_file"; then
    printf 'Expected exactly one port setting in mTCP config: %s\n' "$source_config" >&2
    exit 1
fi

process_running()
{
    [[ -n "$server_pid" ]] || return 1
    kill -0 "$server_pid" 2>/dev/null || return 1
    local process_state
    process_state="$(ps -o stat= -p "$server_pid" 2>/dev/null || true)"
    [[ -n "$process_state" && "$process_state" != Z* ]]
}

print_server_log()
{
    if [[ -s "$log_file" ]]; then
        printf '\n--- mTCP server stdout/stderr ---\n' >&2
        cat -- "$log_file" >&2
        printf '%s\n' '--- end server log ---' >&2
    fi
}

cleanup()
{
    if process_running; then
        kill -TERM "$server_pid" 2>/dev/null || true
        for ((attempt = 0; attempt < 50; attempt++)); do
            process_running || break
            sleep 0.1
        done
        if process_running; then
            printf 'Server PID %s did not stop during cleanup.\n' "$server_pid" >&2
            print_server_log
        fi
    fi

    if [[ "$keep_logs" == 1 ]]; then
        printf 'Server logs and temporary config retained in %s\n' "$temp_dir" >&2
    else
        rm -f -- "$log_file" "$config_file"
        rmdir -- "$temp_dir" 2>/dev/null || true
    fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

ip netns exec "$server_namespace" \
    "$launcher" "$config_file" "$server_ipv4" "$port" >"$log_file" 2>&1 &
server_pid=$!

ready=0
for ((attempt = 0; attempt < 10#$startup_timeout * 10; attempt++)); do
    if ! process_running; then
        printf 'mTCP server exited before becoming reachable.\n' >&2
        print_server_log
        exit 1
    fi
    if ip netns exec "$client_namespace" \
        python3 -c 'import socket, sys; s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=0.3); s.close()' \
        "$server_ipv4" "$port" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.1
done

if [[ "$ready" != 1 ]]; then
    printf 'mTCP server did not become reachable within %s seconds.\n' "$startup_timeout" >&2
    print_server_log
    exit 1
fi

if ! ip netns exec "$client_namespace" \
    python3 "$client" "$server_ipv4" "$port"; then
    print_server_log
    exit 1
fi

kill -TERM "$server_pid"
for ((attempt = 0; attempt < 50; attempt++)); do
    process_running || break
    sleep 0.1
done
if process_running; then
    printf 'mTCP server did not shut down within 5 seconds.\n' >&2
    print_server_log
    exit 1
fi
if wait "$server_pid"; then
    server_pid=""
else
    server_status=$?
    printf 'mTCP server exited with status %s during shutdown.\n' "$server_status" >&2
    print_server_log
    exit 1
fi

printf 'PASS mTCP integration smoke test\n'
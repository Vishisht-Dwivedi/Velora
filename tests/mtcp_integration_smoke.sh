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
capture_tool="$repo_root/tests/capture_af_xdp_traffic.py"
environment_helper="$repo_root/scripts/af_xdp_test_env.sh"
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
if [[ ! -f "$capture_tool" ]]; then
    printf 'Packet capture helper is missing: %s\n' "$capture_tool" >&2
    exit 1
fi
if [[ ! -x "$environment_helper" ]]; then
    printf 'AF_XDP test environment helper is missing or not executable: %s\n' \
        "$environment_helper" >&2
    exit 1
fi

printf '[PREFLIGHT] Checking approved namespaces, veth pair, queues, XDP, routes, processes, listener, and offloads.\n'
ignored_processes="$BASHPID"
ancestor_pid="$PPID"
while [[ "$ancestor_pid" =~ ^[0-9]+$ ]] && (( ancestor_pid > 1 )); do
    ignored_processes+=" $ancestor_pid"
    ancestor_pid="$(ps -o ppid= -p "$ancestor_pid" 2>/dev/null | tr -d ' ' || true)"
done
if ! VELORA_TEST_PORT="$port" VELORA_ENV_IGNORE_PIDS="${ignored_processes// /,}" \
    "$environment_helper" check; then
    printf '[PREFLIGHT] FAILED; refusing to launch capture or server.\n' >&2
    exit 1
fi
printf '[PREFLIGHT] PASSED; client TX checksum offload is off.\n'

temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/velora-mtcp-smoke.XXXXXX")"
log_file="$temp_dir/server.log"
pcap_file="$temp_dir/client-vmtcp1-peer.pcap"
config_file="$temp_dir/afxdp_test.conf"
runtime_root="$temp_dir/runtime"
runtime_dir="$runtime_root/mtcp/src"
server_pid=""
capture_pid=""

mkdir -p "$runtime_dir"
rm -rf -- "$runtime_dir/config"
cp -a "$repo_root/.deps/mtcp-af_xdp/source/mtcp/src/config" "$runtime_dir/config"
cat > "$runtime_dir/config/route.conf" <<EOF
ROUTES 1
198.18.0.0/30 $server_interface
EOF
cat > "$runtime_dir/config/arp.conf" <<EOF
ARP_ENTRY 1
$client_ipv4/32 $client_mac
EOF
ln -s "$repo_root/.deps/mtcp-af_xdp/source/afxdp" \
    "$runtime_root/afxdp"

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

stop_capture()
{
    if [[ -n "$capture_pid" ]] && kill -0 "$capture_pid" 2>/dev/null; then
        kill -TERM "$capture_pid" 2>/dev/null || true
        wait "$capture_pid" 2>/dev/null || true
    fi
    capture_pid=""
}

report_capture_evidence()
{
    local capture_size
    if [[ ! -f "$pcap_file" ]]; then
        printf '[CAPTURE] No pcap file was produced; no packet evidence available.\n'
        return
    fi
    capture_size="$(stat -c '%s' "$pcap_file")"
    if (( capture_size > 24 )); then
        printf '[CAPTURE] Pcap contains packet-record data (%s bytes); packet checksums are not validated by this capture.\n' \
            "$capture_size"
    else
        printf '[CAPTURE] Pcap is header-only or empty (%s bytes); not treating it as packet evidence.\n' \
            "$capture_size"
    fi
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
    stop_capture
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
        printf 'Server logs, mTCP logs, pcap, and temporary config retained in %s\n' "$temp_dir" >&2
    else
        rm -f -- "$log_file" "$config_file"
        rmdir -- "$temp_dir" 2>/dev/null || true
    fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf '[CAPTURE] Starting packet capture on %s/%s.\n' "$client_namespace" "$client_interface"
ip netns exec "$client_namespace" \
    python3 "$capture_tool" "$client_interface" "$pcap_file" "$port" &
capture_pid=$!

capture_ready=0
capture_deadline=$((SECONDS + 5))
while (( SECONDS < capture_deadline )); do
    if ! kill -0 "$capture_pid" 2>/dev/null; then
        printf '[CAPTURE] FAILED before packet socket became ready.\n' >&2
        wait "$capture_pid" || true
        capture_pid=""
        exit 1
    fi
    if [[ -f "$pcap_file" ]] && (( $(stat -c '%s' "$pcap_file") >= 24 )); then
        capture_ready=1
        break
    fi
    sleep 0.05
done
if [[ "$capture_ready" != 1 ]]; then
    printf '[CAPTURE] FAILED: packet socket did not arm within 5 seconds.\n' >&2
    stop_capture
    report_capture_evidence
    exit 1
fi

printf '[SERVER] Starting mTCP server process.\n'
ip netns exec "$server_namespace" \
    env MTCP_RUNTIME_DIR="$runtime_dir" \
    "$launcher" "$config_file" "$server_ipv4" "$port" >"$log_file" 2>&1 &
server_pid=$!
printf '[SERVER] Process started (pid=%s); waiting for listener-ready log.\n' "$server_pid"

listener_ready=0
last_probe_error="no probe attempt completed"
deadline=$((SECONDS + 10#$startup_timeout))
while (( SECONDS < deadline )); do
    if ! process_running; then
        printf '[SERVER] FAILED: process exited before listener-ready log.\n' >&2
        print_server_log
        exit 1
    fi
    if [[ -f "$runtime_dir/log.txt" ]] &&
        grep -F "listener ready" "$runtime_dir/log.txt" | grep -Fq "port=$port"; then
        listener_ready=1
        break
    fi
    sleep 0.1
done

if [[ "$listener_ready" != 1 ]]; then
    printf '[SERVER] FAILED: listener-ready log not observed within %s seconds.\n' \
        "$startup_timeout" >&2
    print_server_log
    exit 1
fi
printf '[SERVER] Listener setup succeeded on %s:%s (mTCP log confirmed).\n' \
    "$server_ipv4" "$port"

tcp_ready=0
while (( SECONDS < deadline )); do
    if ! process_running; then
        printf '[TCP] Readiness probe FAILED: mTCP server exited.\n' >&2
        print_server_log
        exit 1
    fi
    if probe_output="$(ip netns exec "$client_namespace" \
        python3 -c 'import socket, sys; s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=0.3); s.close()' \
        "$server_ipv4" "$port" 2>&1)"; then
        tcp_ready=1
        break
    else
        last_probe_error="$probe_output"
    fi
    sleep 0.1
done

if [[ "$tcp_ready" != 1 ]]; then
    printf '[TCP] Readiness probe FAILED within %s seconds: %s\n' \
        "$startup_timeout" "$last_probe_error" >&2
    stop_capture
    report_capture_evidence
    print_server_log
    exit 1
fi
printf '[TCP] Readiness probe succeeded for %s:%s; this is not a protocol correctness result.\n' \
    "$server_ipv4" "$port"

printf '[PROTOCOL] Running CONNECT_ACK, PING/PONG, pipeline, and fragmented-header checks.\n'
if ip netns exec "$client_namespace" python3 "$client" "$server_ipv4" "$port"; then
    printf '[PROTOCOL] PASSED: protocol client validated all implemented checks.\n'
else
    client_status=$?
    printf '[PROTOCOL] FAILED: client exit status %s.\n' "$client_status" >&2
    print_server_log
    exit 1
fi

stop_capture
report_capture_evidence
if [[ "$keep_logs" == 1 ]]; then
    printf '[CAPTURE] Artifact path: %s\n' "$pcap_file"
fi

printf '[SHUTDOWN] Requesting graceful server shutdown.\n'
if ! kill -TERM "$server_pid" 2>/dev/null; then
    printf '[SHUTDOWN] FAILED: could not send SIGTERM to server PID %s.\n' "$server_pid" >&2
    print_server_log
    exit 1
fi
for ((attempt = 0; attempt < 50; attempt++)); do
    process_running || break
    sleep 0.1
done
if process_running; then
    printf '[SHUTDOWN] FAILED: server did not stop within 5 seconds.\n' >&2
    print_server_log
    exit 1
fi
if wait "$server_pid"; then
    server_pid=""
else
    server_status=$?
    printf '[SHUTDOWN] FAILED: server exited with status %s.\n' "$server_status" >&2
    print_server_log
    exit 1
fi
printf '[SHUTDOWN] PASSED: server exited cleanly.\n'

printf 'PASS mTCP integration smoke test\n'
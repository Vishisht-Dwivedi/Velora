#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/.." && pwd -P)"
config_default="$repo_root/.deps/mtcp-af_xdp/source/mtcp/src/config/afxdp_test.conf"
server_binary="$repo_root/build/velora-mtcp"
launcher="$repo_root/scripts/run-velora-mtcp.sh"
client="$repo_root/tests/mtcp_protocol_client.py"

diag_dir="${1:-$repo_root/diagnostics/velora-mtcp-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$diag_dir"

env_file="$diag_dir/environment.txt"
log_file="$diag_dir/server.log"
client_log="$diag_dir/client.log"
build_log="$diag_dir/build.log"
status_file="$diag_dir/status.txt"

{
    echo "timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "uid=$(id -u)"
    echo "user=$(id -un)"
    echo "cwd=$PWD"
    echo "repo_root=$repo_root"
    echo "hostname=${HOSTNAME:-$(uname -n 2>/dev/null || printf 'unknown')}"
    echo "--- git status ---"
    git -C "$repo_root" status --short
    echo "--- git rev-parse ---"
    git -C "$repo_root" rev-parse HEAD
    echo "--- namespace check ---"
    if command -v ip >/dev/null 2>&1; then
        ip netns list 2>&1 || true
    else
        echo "ip command unavailable"
    fi
    echo "--- config check ---"
    ls -l "$config_default" 2>&1 || true
} > "$env_file"

printf 'Build provenance: %s\n' "$build_log"
(
    cd "$repo_root"
    make velora-mtcp
) > "$build_log" 2>&1 || {
    printf 'BUILD_FAILED\n' > "$status_file"
    printf 'Build failed; see %s\n' "$build_log" >&2
    exit 1
}

printf 'BUILD_OK\n' > "$status_file"

if [[ -f "$launcher" && -x "$launcher" && -f "$client" && -f "$config_default" ]]; then
    if ip netns exec velora-af-test true >/dev/null 2>&1; then
        ip netns exec velora-af-test \
            "$launcher" "$config_default" 198.18.0.1 22409 >"$log_file" 2>&1 &
        server_pid=$!
        echo "$server_pid" > "$diag_dir/server.pid"
        sleep 1
        if kill -0 "$server_pid" 2>/dev/null; then
            ip netns exec velora-af-test python3 "$client" 198.18.0.1 22409 >"$client_log" 2>&1 || true
            kill -TERM "$server_pid" 2>/dev/null || true
            wait "$server_pid" 2>/dev/null || true
            printf 'REPRO_ATTEMPTED\n' > "$diag_dir/repro_status.txt"
        else
            printf 'SERVER_EXITED_BEFORE_CLIENT\n' > "$diag_dir/repro_status.txt"
        fi
    else
        printf 'NETNS_UNAVAILABLE\n' > "$diag_dir/repro_status.txt"
        printf 'The current session cannot access the AF_XDP namespaces required for the privileged runtime reproduction.\n' > "$diag_dir/limitation.txt"
    fi
else
    printf 'MISSING_ARTIFACTS\n' > "$diag_dir/repro_status.txt"
fi

printf 'Diagnostic directory: %s\n' "$diag_dir"

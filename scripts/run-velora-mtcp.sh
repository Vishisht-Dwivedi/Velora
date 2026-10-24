#!/usr/bin/env bash
set -euo pipefail

# Usage: scripts/run-velora-mtcp.sh <config-file> <bind-ipv4> <port>

if [[ $# -ne 3 ]]; then
    printf 'Usage: %s <config-file> <bind-ipv4> <port>\n' "$0" >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/.." && pwd -P)"
server="$repo_root/build/velora-mtcp"
config_file="$1"

if [[ ! -x "$server" ]]; then
    printf 'mTCP server executable is missing: %s\n' "$server" >&2
    exit 1
fi
if [[ ! -f "$config_file" ]]; then
    printf 'mTCP config file does not exist: %s\n' "$config_file" >&2
    exit 1
fi

config_dir="$(cd -- "$(dirname -- "$config_file")" && pwd -P)"
config_file="$config_dir/$(basename -- "$config_file")"
runtime_dir="$repo_root/.deps/mtcp-af_xdp/source/mtcp/src"
if [[ ! -d "$runtime_dir" ]]; then
    printf 'Pinned mTCP runtime directory is missing: %s\n' "$runtime_dir" >&2
    exit 1
fi

cd -- "$runtime_dir"
exec "$server" "$config_file" "$2" "$3"
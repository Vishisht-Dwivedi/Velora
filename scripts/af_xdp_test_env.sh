#!/usr/bin/env bash
set -euo pipefail

server_namespace="velora-af-server"
client_namespace="velora-af-test1"
server_interface="vmtcp1"
client_interface="vmtcp1-peer"
server_ipv4="198.18.0.1"
client_ipv4="198.18.0.2"
server_mac="02:00:00:00:00:01"
client_mac="d2:ce:d7:4a:a8:37"
ownership_alias="velora-af-test-env"
test_port="${VELORA_TEST_PORT:-22409}"
ignored_pid="${VELORA_ENV_IGNORE_PID:-}"
ignored_pids=",${VELORA_ENV_IGNORE_PIDS:-$ignored_pid},"
failed=0

if [[ ! "$test_port" =~ ^[0-9]+$ ]] || (( 10#$test_port < 1 || 10#$test_port > 65535 )); then
    printf 'Invalid test port: %s\n' "$test_port" >&2
    exit 2
fi

usage()
{
    printf 'Usage: %s {up|check|down}\n' "$0" >&2
}

fail()
{
    printf 'ERROR: %s\n' "$*" >&2
    failed=1
}

require_root()
{
    if [[ "$EUID" -ne 0 ]]; then
        printf 'Run this helper as root (for example, sudo %s check).\n' "$0" >&2
        exit 1
    fi
}

require_namespaces()
{
    local namespace
    for namespace in "$server_namespace" "$client_namespace"; do
        if ! ip netns list | awk '{print $1}' | grep -Fxq "$namespace"; then
            printf 'Required existing namespace is missing: %s\n' "$namespace" >&2
            return 1
        fi
    done
}

check_test_processes()
{
    local matches pid state active=0
    matches="$(pgrep -af '(^|[ /])velora-mtcp([[:space:]]|$)|(^|[ /])scripts/run-velora-mtcp[.]sh|(^|[ /])tests/mtcp_integration_smoke[.]sh|(^|[ /])tests/mtcp_protocol_client[.]py|(^|[ /])tests/capture_af_xdp_traffic[.]py' || true)"
    while read -r pid _; do
        [[ -n "$pid" ]] || continue
        [[ "$ignored_pids" == *",$pid,"* ]] && continue
        state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
        [[ -n "$state" && "$state" != T* && "$state" != Z* ]] || continue
        ps -o pid=,stat=,args= -p "$pid" >&2
        active=1
    done <<<"$matches"
    (( active == 0 ))
}

check_listener()
{
    local listeners
    listeners="$(ip netns exec "$server_namespace" ss -H -ltn "sport = :$test_port")"
    if [[ -n "$listeners" ]]; then
        printf 'TCP/%s listener exists in %s:\n%s\n' \
            "$test_port" "$server_namespace" "$listeners" >&2
        return 1
    fi
}

link_exists()
{
    ip -n "$1" link show dev "$2" >/dev/null 2>&1
}

name_exists_outside_target()
{
    local interface="$1"
    local target_namespace="$2"
    local namespace

    if ip link show dev "$interface" >/dev/null 2>&1; then
        printf 'Interface name %s already exists in the host namespace.\n' "$interface" >&2
        return 0
    fi
    while read -r namespace _; do
        [[ -n "$namespace" && "$namespace" != "$target_namespace" ]] || continue
        if ip -n "$namespace" link show dev "$interface" >/dev/null 2>&1; then
            printf 'Interface name %s already exists in unrelated namespace %s.\n' \
                "$interface" "$namespace" >&2
            return 0
        fi
    done < <(ip netns list)
    return 1
}

has_xdp()
{
    local namespace="$1"
    local interface="$2"
    local link_json

    link_json="$(ip -n "$namespace" -j -d link show dev "$interface")" || return 1
    python3 -c 'import json, sys; links = json.load(sys.stdin); sys.exit(0 if len(links) == 1 and "xdp" in links[0] else 1)' \
        <<<"$link_json"
}

check_no_endpoint_xdp()
{
    local namespace="$1"
    local interface="$2"
    if link_exists "$namespace" "$interface" && has_xdp "$namespace" "$interface"; then
        printf 'XDP attachment on %s/%s:\n' "$namespace" "$interface" >&2
        ip -n "$namespace" -details link show dev "$interface" >&2
        return 1
    fi
}

check_name_available_for_up()
{
    local interface="$1"
    local target_namespace="$2"
    if name_exists_outside_target "$interface" "$target_namespace"; then
        return 1
    fi
    if link_exists "$target_namespace" "$interface"; then
        printf 'Refusing to overwrite existing interface %s/%s.\n' \
            "$target_namespace" "$interface" >&2
        return 1
    fi
}

show_namespace_state()
{
    local namespace="$1"
    printf '\n=== namespace: %s ===\n' "$namespace"
    ip -n "$namespace" -brief link show
    ip -n "$namespace" -brief address show
    printf '%s\n' 'Routes:'
    ip -n "$namespace" route show table all
}

check_endpoint()
{
    local namespace="$1"
    local interface="$2"
    local expected_ipv4="$3"
    local expected_mac="$4"
    local peer_ipv4="$5"
    local expected_tx="$6"
    local link_json address_json features route_output expected_source
    expected_source="${expected_ipv4%/*}"

    printf '\n--- endpoint %s/%s ---\n' "$namespace" "$interface"
    if ! link_json="$(ip -n "$namespace" -j -d link show dev "$interface" 2>&1)"; then
        fail "cannot inspect $namespace/$interface: $link_json"
        return
    fi
    ip -n "$namespace" -details link show dev "$interface"
    if has_xdp "$namespace" "$interface"; then
        printf '%s\n' 'XDP: attached (mismatch)'
    else
        printf '%s\n' 'XDP: none'
    fi
    address_json="$(ip -n "$namespace" -j -4 address show dev "$interface")"
    ip -n "$namespace" -brief address show dev "$interface"
    printf '%s\n' 'Queue configuration:'
    ip netns exec "$namespace" ethtool -l "$interface"
    printf '%s\n' 'Offload features (complete ethtool -k output):'
    features="$(ip netns exec "$namespace" ethtool -k "$interface")"
    printf '%s\n' "$features"
    route_output="$(ip -n "$namespace" route get "$peer_ipv4" 2>&1)" || true
    printf 'Route to peer: %s\n' "$route_output"

    if ! python3 -c '
import json, sys
links = json.loads(sys.argv[1])
addresses = json.loads(sys.argv[2])
expected_mac, expected_ipv4 = sys.argv[3:5]
valid = len(links) == 1 and links[0].get("address", "").lower() == expected_mac
valid = valid and links[0].get("linkinfo", {}).get("info_kind") == "veth"
valid = valid and links[0].get("num_rx_queues") == 1 and links[0].get("num_tx_queues") == 1
valid = valid and "UP" in links[0].get("flags", []) and "xdp" not in links[0]
valid = valid and any(
    info.get("local") + "/" + str(info.get("prefixlen")) == expected_ipv4
    for item in addresses for info in item.get("addr_info", [])
)
sys.exit(0 if valid else 1)
' "$link_json" "$address_json" "$expected_mac" "$expected_ipv4"; then
        fail "$namespace/$interface does not match expected veth, MAC, IPv4, one-queue, UP, no-XDP state"
    fi
    if [[ "$(printf '%s\n' "$features" | awk -F': ' '/^tx-checksumming:/{print $2}')" != "$expected_tx" ]]; then
        fail "$namespace/$interface tx-checksumming is not $expected_tx"
    fi
    if [[ "$route_output" != *"dev $interface"* || "$route_output" != *"src $expected_source"* ]]; then
        fail "route to $peer_ipv4 does not use $interface with source $expected_source"
    fi
}

check_environment()
{
    local server_link client_link
    require_namespaces || return 1
    show_namespace_state "$server_namespace"
    show_namespace_state "$client_namespace"
    if ! ip -n "$server_namespace" -j link show dev lo |
        python3 -c 'import json, sys; links = json.load(sys.stdin); sys.exit(0 if len(links) == 1 and "UP" in links[0].get("flags", []) else 1)'; then
        fail "loopback is not up in $server_namespace"
    fi
    if ! ip -n "$client_namespace" -j link show dev lo |
        python3 -c 'import json, sys; links = json.load(sys.stdin); sys.exit(0 if len(links) == 1 and "UP" in links[0].get("flags", []) else 1)'; then
        fail "loopback is not up in $client_namespace"
    fi
    check_endpoint "$server_namespace" "$server_interface" \
        "$server_ipv4/30" "$server_mac" "$client_ipv4" on
    check_endpoint "$client_namespace" "$client_interface" \
        "$client_ipv4/30" "$client_mac" "$server_ipv4" off
    server_link="$(ip -n "$server_namespace" -j -d link show dev "$server_interface")"
    client_link="$(ip -n "$client_namespace" -j -d link show dev "$client_interface")"
    if ! python3 -c '
import json, sys
server, client = (json.loads(value)[0] for value in sys.argv[1:3])
sys.exit(0 if server.get("link_index") == client.get("ifindex") and client.get("link_index") == server.get("ifindex") else 1)
' "$server_link" "$client_link"; then
        fail 'the configured interfaces are not a matching veth pair'
    fi

    printf '\n=== test process/listener checks ===\n'
    if ! check_test_processes; then
        fail 'test process check failed'
    else
        printf '%s\n' 'No Velora/mTCP integration process is active.'
    fi
    if ! check_listener; then
        fail "TCP/$test_port listener check failed"
    else
        printf 'No TCP/%s listener exists in %s.\n' "$test_port" "$server_namespace"
    fi

    if (( failed != 0 )); then
        printf '\nFAIL: AF_XDP test environment mismatch.\n' >&2
        return 1
    fi
    printf '\nPASS: AF_XDP test environment matches the documented profile.\n'
}

verify_owned_endpoint()
{
    local namespace="$1"
    local interface="$2"
    local expected_ipv4="$3"
    local expected_mac="$4"
    local link_json address_json

    link_json="$(ip -n "$namespace" -j -d link show dev "$interface")" || {
        printf 'Expected owned interface is missing: %s/%s\n' "$namespace" "$interface" >&2
        return 1
    }
    address_json="$(ip -n "$namespace" -j -4 address show dev "$interface")"
    python3 -c '
import json, sys
links = json.loads(sys.argv[1])
addresses = json.loads(sys.argv[2])
expected_mac, expected_ipv4, expected_alias = sys.argv[3:6]
valid = len(links) == 1 and links[0].get("ifalias") == expected_alias
valid = valid and links[0].get("address", "").lower() == expected_mac
valid = valid and links[0].get("linkinfo", {}).get("info_kind") == "veth"
valid = valid and links[0].get("num_rx_queues") == 1 and links[0].get("num_tx_queues") == 1
valid = valid and "xdp" not in links[0]
valid = valid and any(
    info.get("local") + "/" + str(info.get("prefixlen")) == expected_ipv4
    for item in addresses for info in item.get("addr_info", [])
)
sys.exit(0 if valid else 1)
' "$link_json" "$address_json" "$expected_mac" "$expected_ipv4" "$ownership_alias"
}

up()
{
    require_namespaces
    check_test_processes || {
        printf 'Refusing to change topology while a test process is active.\n' >&2
        return 1
    }
    check_listener || {
        printf 'Refusing to change topology while TCP/%s is listening.\n' "$test_port" >&2
        return 1
    }
    check_name_available_for_up "$server_interface" "$server_namespace"
    check_name_available_for_up "$client_interface" "$client_namespace"
    check_no_endpoint_xdp "$server_namespace" "$server_interface"
    check_no_endpoint_xdp "$client_namespace" "$client_interface"

    trap 'status=$?; printf "ERROR: setup failed at line %s (status %s); inspect only %s/%s and %s/%s for partial setup before retrying.\\n" "$LINENO" "$status" "$server_namespace" "$server_interface" "$client_namespace" "$client_interface" >&2; exit "$status"' ERR
    ip link add "$server_interface" numrxqueues 1 numtxqueues 1 type veth \
        peer name "$client_interface" numrxqueues 1 numtxqueues 1
    ip link set dev "$server_interface" alias "$ownership_alias"
    ip link set dev "$client_interface" alias "$ownership_alias"
    ip link set dev "$server_interface" netns "$server_namespace"
    ip link set dev "$client_interface" netns "$client_namespace"
    ip -n "$server_namespace" link set lo up
    ip -n "$client_namespace" link set lo up
    ip -n "$server_namespace" link set dev "$server_interface" address "$server_mac"
    ip -n "$client_namespace" link set dev "$client_interface" address "$client_mac"
    ip -n "$server_namespace" address add "$server_ipv4/30" dev "$server_interface"
    ip -n "$client_namespace" address add "$client_ipv4/30" dev "$client_interface"
    ip -n "$server_namespace" link set "$server_interface" up
    ip -n "$client_namespace" link set "$client_interface" up
    ip netns exec "$client_namespace" ethtool -K "$client_interface" tx off
    trap - ERR
    printf 'Created and configured only %s/%s and %s/%s.\n' \
        "$server_namespace" "$server_interface" "$client_namespace" "$client_interface"
    check_environment
}

down()
{
    require_namespaces
    check_test_processes || {
        printf 'Refusing to remove topology while a test process is active.\n' >&2
        return 1
    }
    check_listener || {
        printf 'Refusing to remove topology while TCP/%s is listening.\n' "$test_port" >&2
        return 1
    }
    verify_owned_endpoint "$server_namespace" "$server_interface" \
        "$server_ipv4/30" "$server_mac" || {
        printf 'Refusing to remove an endpoint without the helper ownership marker and expected configuration.\n' >&2
        return 1
    }
    verify_owned_endpoint "$client_namespace" "$client_interface" \
        "$client_ipv4/30" "$client_mac" || {
        printf 'Refusing to remove an endpoint without the helper ownership marker and expected configuration.\n' >&2
        return 1
    }
    server_link="$(ip -n "$server_namespace" -j -d link show dev "$server_interface")"
    client_link="$(ip -n "$client_namespace" -j -d link show dev "$client_interface")"
    python3 -c '
import json, sys
server, client = (json.loads(value)[0] for value in sys.argv[1:3])
sys.exit(0 if server.get("link_index") == client.get("ifindex") and client.get("link_index") == server.get("ifindex") else 1)
' "$server_link" "$client_link" || {
        printf 'Refusing to remove interfaces that are not the expected veth pair.\n' >&2
        return 1
    }
    printf 'Removing owned veth pair %s/%s <-> %s/%s.\n' \
        "$server_namespace" "$server_interface" "$client_namespace" "$client_interface"
    ip -n "$server_namespace" link delete dev "$server_interface"
}

main()
{
    require_root
    if [[ $# -ne 1 ]]; then
        usage
        return 2
    fi
    case "$1" in
        up) up ;;
        check) check_environment ;;
        down) down ;;
        *) usage; return 2 ;;
    esac
}

main "$@"
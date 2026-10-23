#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK_FILE="$ROOT_DIR/dependencies/mtcp-af_xdp.lock"
PATCH_FILE="$ROOT_DIR/dependencies/patches/0001-clang-remove-obsolete-c-flag.patch"
OWNERSHIP_PATCH="$ROOT_DIR/dependencies/patches/0002-xdp-ownership-and-failure-propagation.patch"
SOCKET_MAP_PATCH="$ROOT_DIR/dependencies/patches/0003-af-xdp-socket-map-registration.patch"
OWNED_LIFECYCLE_PATCH="$ROOT_DIR/dependencies/patches/0004-af-xdp-owned-lifecycle.patch"
QUEUE_TX_PATCH="$ROOT_DIR/dependencies/patches/0005-af-xdp-queue-and-tx-safety.patch"
BUILD_ROOT="$ROOT_DIR/.deps/mtcp-af_xdp"
SOURCE_DIR="$BUILD_ROOT/source"

# shellcheck source=dependencies/mtcp-af_xdp.lock
source "$LOCK_FILE"

for tool in git make gcc clang llc pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'Required tool not found: %s\n' "$tool" >&2
        exit 1
    }
done

pkg-config --exists libelf || {
    printf 'Missing libelf development files (pkg-config libelf).\n' >&2
    exit 1
}

mkdir -p "$BUILD_ROOT"
if [[ ! -d "$SOURCE_DIR/.git" ]]; then
    git clone --no-checkout "$MTCP_REPOSITORY" "$SOURCE_DIR"
    git -C "$SOURCE_DIR" fetch --depth 1 origin "$MTCP_REVISION"
    git -C "$SOURCE_DIR" checkout --detach FETCH_HEAD
fi

actual_revision="$(git -C "$SOURCE_DIR" rev-parse HEAD)"
if [[ "$actual_revision" != "$MTCP_REVISION" ]]; then
    printf 'Pinned revision mismatch: expected %s, found %s\n' \
        "$MTCP_REVISION" "$actual_revision" >&2
    exit 1
fi

if git -C "$SOURCE_DIR" apply --reverse --check --ignore-whitespace "$PATCH_FILE"; then
    printf 'Compatibility patch already applied.\n'
elif git -C "$SOURCE_DIR" apply --check --ignore-whitespace "$PATCH_FILE"; then
    git -C "$SOURCE_DIR" apply --ignore-whitespace "$PATCH_FILE"
else
    printf 'Dependency source has unexpected changes; refusing to patch it.\n' >&2
    exit 1
fi

if git -C "$SOURCE_DIR" apply --reverse --check --unidiff-zero "$OWNERSHIP_PATCH"; then
    printf 'XDP ownership patch already applied.\n'
elif git -C "$SOURCE_DIR" apply --check --unidiff-zero "$OWNERSHIP_PATCH"; then
    git -C "$SOURCE_DIR" apply --unidiff-zero "$OWNERSHIP_PATCH"
else
    printf 'Dependency source has unexpected XDP ownership changes; refusing to patch it.\n' >&2
    exit 1
fi

    if git -C "$SOURCE_DIR" apply --ignore-whitespace --recount --reverse --check "$OWNED_LIFECYCLE_PATCH"; then
    printf 'AF_XDP owned-lifecycle patch already applied.\n'
    if ! grep -Fq 'bpf_map_update_elem(axpc->xsks_map_fd' \
        "$SOURCE_DIR/mtcp/src/afxdp_module.c"; then
        printf 'Owned-lifecycle patch is present without XSKMAP registration.\n' >&2
        exit 1
    fi
    lifecycle_already_applied=1
else
    if git -C "$SOURCE_DIR" apply --ignore-whitespace --recount --reverse --check "$SOCKET_MAP_PATCH"; then
        printf 'AF_XDP socket-map registration patch already applied.\n'
    elif git -C "$SOURCE_DIR" apply --ignore-whitespace --recount --check "$SOCKET_MAP_PATCH"; then
        git -C "$SOURCE_DIR" apply --ignore-whitespace --recount "$SOCKET_MAP_PATCH"
    else
        printf 'Dependency source has unexpected AF_XDP module changes; refusing to patch it.\n' >&2
        exit 1
    fi

    if git -C "$SOURCE_DIR" apply --ignore-whitespace --recount --check "$OWNED_LIFECYCLE_PATCH"; then
        git -C "$SOURCE_DIR" apply --ignore-whitespace --recount "$OWNED_LIFECYCLE_PATCH"
    else
        printf 'Dependency source has unexpected AF_XDP lifecycle changes; refusing to patch it.\n' >&2
        exit 1
    fi
fi

if grep -Fq 'get_rx_queue_count' "$SOURCE_DIR/mtcp/src/afxdp_module.c" &&
   grep -Fq 'afxdp_tx_submit_pending' "$SOURCE_DIR/mtcp/src/afxdp_module.c" &&
   grep -Fq 'xdp_action_counters' "$SOURCE_DIR/afxdp/mtcp_xdp_pktio/afxdp_kern.c"; then
    printf 'AF_XDP queue/TX safety patch already applied.\n'
elif git -C "$SOURCE_DIR" apply --ignore-whitespace --unidiff-zero --recount --check "$QUEUE_TX_PATCH"; then
    git -C "$SOURCE_DIR" apply --ignore-whitespace --unidiff-zero --recount "$QUEUE_TX_PATCH"
else
    printf 'Dependency source has unexpected AF_XDP queue/TX changes; refusing to patch it.\n' >&2
    exit 1
fi

printf 'mTCP repository: %s\n' "$MTCP_REPOSITORY"
printf 'mTCP revision:   %s\n' "$actual_revision"
printf 'Compiler:        %s\n' "$(gcc --version | head -n 1)"
printf 'Clang:           %s\n' "$(clang --version | head -n 1)"
printf 'LLVM:            %s\n' "$(llc --version | head -n 1)"
printf 'libelf:          %s\n' "$(pkg-config --modversion libelf)"

make -C "$SOURCE_DIR/afxdp/libbpf/src"
make -C "$SOURCE_DIR/afxdp/libbpf/src" install_headers DESTDIR=build
make -C "$SOURCE_DIR/afxdp/common"
make -C "$SOURCE_DIR/afxdp/mtcp_xdp_pktio"

(
    cd "$SOURCE_DIR/mtcp/src"
    ./compile
)

required_artifacts=(
    "$SOURCE_DIR/afxdp/libbpf/src/libbpf.a"
    "$SOURCE_DIR/afxdp/common/common_params.o"
    "$SOURCE_DIR/afxdp/common/common_user_bpf_xdp.o"
    "$SOURCE_DIR/afxdp/mtcp_xdp_pktio/afxdp_kern.o"
    "$SOURCE_DIR/mtcp/lib/libmtcp.a"
)
for artifact in "${required_artifacts[@]}"; do
    if [[ ! -s "$artifact" ]]; then
        printf 'Required build artifact missing or empty: %s\n' "$artifact" >&2
        exit 1
    fi
done

printf '\nStage 0 artifacts built successfully:\n'
for artifact in "${required_artifacts[@]}"; do
    printf '  %s\n' "${artifact#"$ROOT_DIR/"}"
done

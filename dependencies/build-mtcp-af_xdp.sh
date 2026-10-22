#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK_FILE="$ROOT_DIR/dependencies/mtcp-af_xdp.lock"
PATCH_FILE="$ROOT_DIR/dependencies/patches/0001-clang-remove-obsolete-c-flag.patch"
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

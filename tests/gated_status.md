# Gated Implementation Status

## Stage 1: Partial AF_XDP module hardening

Maintained patches:

- `dependencies/patches/0002-xdp-ownership-and-failure-propagation.patch`
- `dependencies/patches/0003-af-xdp-socket-map-registration.patch`
- `dependencies/patches/0004-af-xdp-owned-lifecycle.patch`

The XDP attachment helper no longer detaches an existing program to change
attachment mode; it sets `XDP_FLAGS_UPDATE_IF_NOEXIST`. Detachment now refuses
an absent expected program ID and refuses to remove a different active ID.
The module now checks the `xsks_map`, registers the socket fd under queue 0,
and rejects configurations other than one interface and queue 0/context 0. It
no longer invokes `ethtool`; it records the active program ID, unregisters the
queue before socket teardown, and requests detach only for that recorded ID.
The build script applies all three Stage 1 patches idempotently after the Stage
0 patch.

Validation: `make mtcp-af_xdp` passed after applying all patches to the pinned
revision, compiled the modified AF_XDP module, and rebuilt all required Stage 0
artifacts. The builder also recognized all patches as already applied on a
repeat run. Legacy compiler warnings remain. No XDP program was loaded and no
interface changed.

**Gate 1: NOT PASSED.** The module still has incomplete `afxdp_select()`, unsafe
TX reservation/completion handling, unverified RX/FILL frame ownership, and no
reliable partial-initialization rollback. Teardown logs but cannot recover if
queue unregister or owned-program detach fails. No packet-path runtime test
has been attempted. Stop before Stage 2 or Velora integration.

## Stage 0: Pinned mTCP AF_XDP build

**Gate 0: PASS** on 2026-10-22 for the required build artifacts only.

The dependency is pinned in `dependencies/mtcp-af_xdp.lock` to
`https://github.com/ziten98/mtcp.git` at
`a7a5b7b07379bfd5d290a3fcb400e06358160cf0`. The opt-in command is
`make mtcp-af_xdp`; it clones into the ignored `.deps/mtcp-af_xdp/source/`
directory, applies `dependencies/patches/0001-clang-remove-obsolete-c-flag.patch`,
and builds locally. It does not install globally, invoke `sudo`, attach XDP,
or modify network interfaces. Normal `debug` and `release` targets do not
depend on this optional target.

Build environment: GCC 16.2.1, Clang 23.1.3, LLVM `llc` 0.197, libelf 0.197.
The required artifacts were built twice from the pinned checkout:

- `afxdp/libbpf/src/libbpf.a`
- `afxdp/common/common_params.o`
- `afxdp/common/common_user_bpf_xdp.o`
- `afxdp/mtcp_xdp_pktio/afxdp_kern.o`
- `mtcp/lib/libmtcp.a`, including `afxdp_module.o` and its AF_XDP entry points

Patch: remove the obsolete `-c` option from the Clang `-emit-llvm` command in
`afxdp/common/common.mk`. Clang 23 rejects that combination under the fork's
`-Werror`; LLVM IR generation with `-S -emit-llvm` does not need `-c`. The
build script checks the exact pinned revision, applies the patch idempotently,
refuses unexpected source changes, and verifies each artifact is nonempty.
The legacy mTCP compile emits existing warnings but succeeds.

Validation commands: `bash -n dependencies/build-mtcp-af_xdp.sh`,
`make mtcp-af_xdp` (twice), `nm -A .deps/mtcp-af_xdp/source/mtcp/lib/libmtcp.a
| rg 'afxdp_(init_handle|send_pkts|recv_pkts)|afxdp_module_func'`, and
`git diff --check`.

This pass is only dependency build reproducibility. It does not change the
prior B0/Gate 1 status: no stock example execution, isolated TCP exchange,
XDP attachment, or effective AF_XDP socket mode has been verified.

## A0: Protocol framing

Completed on 2026-10-22. The wire header is 8 bytes in this field order:

| Offset | Field | Size | Encoding |
| --- | --- | ---: | --- |
| 0 | magic | 2 | big-endian |
| 2 | version | 1 | byte |
| 3 | type | 1 | byte |
| 4 | stream ID | 1 | byte |
| 5 | flags | 1 | byte |
| 6 | payload length | 2 | big-endian |

The serializer already encoded 16-bit fields explicitly in network byte order. The parser and response sizing had incorrectly used `sizeof(vr_packet_header_t)`, which is 12 bytes on the tested x86_64 ABI because the C enum and alignment are not wire fields. A shared `VR_HDR_SIZE` now controls serialized offsets. The parser rejects type 0 and out-of-range types. A valid `STREAM_CLOSE` is one-way because the protocol defines no close acknowledgment; it no longer emits an invalid type-0 packet.

Validation:

- `make test-protocol`
- `make test-protocol CFLAGS='-Wall -Wextra -Iinclude -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' DEBUG_FLAGS='-g'`
- `make debug` and `make release`
- `bash tests/overall_single.sh`

The framing tests cover all defined packet types, 8-byte serialization, big-endian values, payload round trips, one-byte header delivery, coalesced packets, a truncated header, a wrapped header, partial payload delivery, invalid type rejection, and close dispatch. ASan/UBSan reported no errors in this exercised slice.

## A1: Preliminary Linux TCP baseline

Host: Arch Linux, kernel `7.2.11-arch1-1`, x86_64; Intel Core i3-7020U, 2 cores / 4 logical CPUs; GCC `16.2.1`; release build flags `-O3 -Wall -Wextra -Iinclude -pthread`.

Workload: `tests/throughput_pingpong.c`, loopback to `127.0.0.1:22409`, 100 TCP connections, 8 outstanding PINGs per connection, 8-byte messages, 10-second measurement; connection setup excluded. Each completion is counted only after a valid PONG. The benchmark records end-to-end loopback RTT in 1-microsecond histogram buckets; values at or above 100,000 microseconds share the overflow bucket.

Build and run:

```sh
make release
gcc -D_POSIX_C_SOURCE=200809L -O3 -Wall -Wextra -Wpedantic \
  -o /tmp/velora-throughput-pingpong tests/throughput_pingpong.c
/tmp/velora-throughput-pingpong 127.0.0.1 22409 100 8 10
```

| Run | Completed PING/PONG | Throughput | RTT p50 / p95 / p99 | Errors | Alive |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 1,895,549 | 189,535/s | 4,070 / 6,774 / 8,656 us | 0 | 100/100 |
| 2 | 1,828,609 | 182,861/s | 4,123 / 7,292 / 9,614 us | 0 | 100/100 |

Server-attached `perf stat` over a 15-second window around each run reported 8.89 and 8.83 seconds of user task-clock, respectively; 1.50B and 1.46B user cycles; 1.07B and 1.03B instructions; 0 context switches and migrations; 145 and 90 page faults. This corresponds to roughly 0.89 logical-CPU-seconds per 10-second workload, but the interval also includes idle time and counters do not attribute time to networking, polling, parsing, allocation, or handlers.

`perf record -g` is blocked by host policy (`perf_event_paranoid=2`), so no call-graph profile or kernel-time attribution is available. These two loopback runs are a preliminary, modest-concurrency baseline, not a capacity target or evidence that kernel TCP is the bottleneck. A1 is incomplete pending an allowed sampled profile and/or finer attribution.

## B0: mTCP AF_XDP feasibility

Inspected `ziten98/mtcp` `master`, commit `a7a5b7b07379bfd5d290a3fcb400e06358160cf0` (2022-01-06), a fork of `mcabranches/mtcp`. The fork README documents AF_XDP testing on Ubuntu 18.04.1 / kernel 5.3.0-61. Its AF_XDP source uses an XSKMAP, queue-index redirection with XDP_PASS when no socket is mapped, and per-mTCP-context queue selection. Source exposes copy (`-c`) and zero-copy (`-z`) requests, but these flags alone do not establish effective runtime mode.

Build evidence in a disposable clone under `/tmp/velora-mtcp-audit`:

- Bundled libbpf built with `make -C afxdp/libbpf/src`; headers staged locally with `DESTDIR=build`.
- AF_XDP common helpers compiled with `make -C afxdp/common`.
- mTCP AF_XDP library compiled with `cd mtcp/src && ./compile`; `mtcp/lib/libmtcp.a` was produced, with legacy compiler warnings.
- The XDP BPF object built using the same LLVM pipeline as the stock rules after omitting the obsolete `-c` option that current Clang 23 rejects with `-emit-llvm` and `-Werror`.
- The stock example did not link under GCC 16: duplicate global definitions (`log_thread_state`, `Options`) remain, among other legacy build issues. The example has not run.

The upstream `compile_afxdp_support` invokes `sudo make install`, so it was not run. No dedicated test interface or veth exists in this environment; `enp3s0` is down and `wlan0` is active. No XDP program was loaded, no interface was attached or reconfigured, and no packet-I/O mode or real TCP exchange was verified.

**Gate 1: NOT PASSED; B0 blocked.** The fork's library and BPF object are buildable with a small toolchain compatibility adjustment, but stock example execution, actual AF_XDP traffic, and effective mode remain unproven. Do not begin B1. Resume only with a disposable isolated topology and after resolving the stock example build failures. No GO/CONDITIONAL GO decision is claimed without the required traffic evidence.
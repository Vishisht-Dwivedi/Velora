# Velora AF_XDP Integration — Engineering Handoff

**Purpose:** allow the next coding agent to resume the AF_XDP + mTCP integration without repeating the investigation, losing prior changes, or accidentally disturbing the host networking setup.

**Last evidence represented here:** repository audit at root commit `9a7781f` on branch `af_xdp`, plus the subsequent agent logs that added the canonical `0005-af-xdp-queue-and-tx-safety.patch` and began improving smoke-test shutdown. The latest provided log does **not** include a final post-shutdown-edit status/test summary. Verify current state before editing.

---

## 1. Project objective — do not lose sight of this

Velora is a C networking project. The current goal is to integrate the pinned mTCP AF_XDP backend and measure the real performance difference when Velora bypasses the kernel networking hot path.

The success path is:

1. Start the intended mTCP + AF_XDP executable.
2. Attach the intended XDP program to the intended test interface.
3. Create UMEM and an AF_XDP socket, and register the XSK in the correct XSKMAP entry.
4. Receive the peer's TCP SYN through AF_XDP and mTCP.
5. Complete SYN → SYN-ACK → ACK and return from `mtcp_accept()`.
6. Receive a valid Velora `CONNECT` frame and send `CONNECT_ACK`.
7. Receive a valid `PING` frame and send `PONG`.
8. Shut down cleanly and release only resources owned by the test.
9. Only then run paired native and mTCP + AF_XDP benchmarks using the same workload.

**A successful build, XDP attach, registered map entry, or `Listening` log is not end-to-end success.** No successful AF_XDP TCP handshake or peer-validated CONNECT_ACK/PONG has been established in the evidence supplied so far. No performance improvement has been measured for a working AF_XDP path.

## 2. Repository and branch state

### Root repository

The audit initially found:

- Branch: `af_xdp`
- Root HEAD: `9a7781f` (reported as one commit beyond previous known `9269438` and equal to `origin/af_xdp` at audit time)
- Root worktree was clean at the start of that audit.
- No root staged or unstaged diff existed at that initial audit point.

Subsequent agent work added/updated the following intended root-worktree files:

- `Makefile` — adds `test-af-xdp-packet-io` target.
- `dependencies/build-mtcp-af_xdp.sh` — applies the canonical patch series and adjusted patch application flags.
- `dependencies/patches/0005-af-xdp-queue-and-tx-safety.patch` — new canonical AF_XDP RX queue policy and TX reservation handling patch.
- `tests/afxdp_packet_io.c` — focused queue-policy/TX-ring regression test.
- `tests/gated_status.md` — records packet-I/O test scope and the remaining live-test gate.

The latest agent excerpt also says the smoke signal handling was changed so the handler is installed only after its pipe exists, and shutdown signals are blocked during mTCP initialization to avoid a startup race. It also says the startup diagnostic in patch `0005` was moved to after XSKMAP registration. **The excerpt does not provide a final exact `git status`, diff, or rerun of all tests after these latest edits.** These changes may have added/modified further files, so inspect status and diffs before assuming the list above is complete.

### Pinned dependency checkout

Dependency source location:

- `.deps/mtcp-af_xdp/source/`
- Canonical pin: `dependencies/mtcp-af_xdp.lock`
- Canonical build script: `dependencies/build-mtcp-af_xdp.sh`
- Canonical patches: `dependencies/patches/0001-...` through `0005-...`

The checkout under `.deps` is ignored/generated state and has pre-existing modified/untracked content from earlier work. The agent deliberately did not make the queue/TX fix only in `.deps`; it placed the fix in the canonical patch series. **Do not clean, reset, or overwrite the ignored checkout.** The canonical patches must be tested against a fresh archive/checkout of the pinned base.

### Mandatory first commands for the next agent

Run from the project root and save the output in the handoff/agent notes:

```bash
git status --short --branch --untracked-files=all
git rev-parse HEAD
git log -10 --oneline --decorate
git diff --stat
git diff --cached --stat

git -C .deps/mtcp-af_xdp/source status --short --untracked-files=all
git -C .deps/mtcp-af_xdp/source rev-parse HEAD
```

Do not run `git reset`, `git clean`, stash, broad reverts, or commands that overwrite current build outputs. Preserve all existing work.

---

## 3. Runtime architecture and where the path can fail

There are two different execution paths:

- The ordinary native Velora server (`src/main.c`, `src/net/tcp_server.c`, reactor/socket utilities).
- The separate mTCP smoke executable (`tests/mtcp_runtime_smoke.c`) linked with `src/net/mtcp_runtime.c` and the pinned mTCP library.

The standard `debug` and `release` source lists exclude `src/net/mtcp_runtime.c`. Therefore, native build success is not proof that the mTCP runtime was compiled or used. The mTCP smoke target is the relevant integration executable at present.

### mTCP/AF_XDP ingress path

```text
smoke main
  -> vr_mtcp_runtime_init()
  -> mtcp_init() / configuration load
  -> context 0 and mTCP worker
  -> configured interface discovery and I/O-module selection
  -> BPF object load and XDP attach
  -> UMEM creation
  -> AF_XDP socket bound to interface/queue
  -> XSKMAP registration
  -> AF_XDP RX polling
  -> ProcessPacket()
  -> Ethernet -> IPv4 -> TCP processing
  -> TCP passive open / handshake
  -> mtcp_accept()
  -> connection registered with mTCP epoll
  -> mtcp_recv() -> Velora ring/parser/dispatch
  -> CONNECT_ACK or PONG queued and passed to mtcp_write()
  -> mTCP TCP/IP/Ethernet output -> AF_XDP TX ring
  -> NIC/veth peer
```

### Main first-party files

- `src/net/mtcp_runtime.c` — runtime lifecycle, listener, accept, event processing, receive/write queues, cleanup.
- `include/velora/mtcp_runtime.h` — runtime interface.
- `tests/mtcp_runtime_smoke.c` — mTCP smoke entry point and its pass/fail conditions.
- `src/protocol/parser.c`, `src/protocol/protocol.c`, `src/protocol/packet.c` — Velora framing and request/reply path.
- `src/conn/conn.c`, `src/conn/ring_buffer.c` — connection and buffering.
- `Makefile` — native targets, smoke target and tests.
- `dependencies/mtcp-af_xdp.lock`, `dependencies/build-mtcp-af_xdp.sh`, `dependencies/patches/` — pinned mTCP/AF_XDP integration state.
- `.deps/mtcp-af_xdp/source/mtcp/src/afxdp_module.c` — upstream AF_XDP interface implementation (generated/ignored; maintain changes in patch files).
- `.deps/mtcp-af_xdp/source/afxdp/mtcp_xdp_pktio/afxdp_kern.c` — XDP redirect logic (generated/ignored; maintain changes in patch files).
- `.deps/mtcp-af_xdp/source/mtcp/src/{core.c,config.c,io_module.c,api.c,eventpoll.c,tcp_in.c,tcp_out.c,arp.c,eth_in.c,eth_out.c,ip_in.c,ip_out.c}` — mTCP runtime/config/protocol path.

Configuration paths in this pinned mTCP version are relative to the process working directory, including `config/route.conf`, `config/arp.conf`, and the AF_XDP BPF object path `../../afxdp/mtcp_xdp_pktio/afxdp_kern.o`. Existing launch commands intentionally start from `.deps/mtcp-af_xdp/source/mtcp/src`.

---

## 4. Findings — confirmed versus still to prove

### A. RX queue policy / XSKMAP mismatch

**Source-level defect/risk confirmed.** The earlier AF_XDP module created and registered only XSK queue key `0`; the XDP program uses `rx_queue_index` to select an XSKMAP entry and returns `XDP_PASS` when the queue has no entry. The live `vxdp0` has four RX queues. A SYN arriving on RX queue 1–3 can bypass mTCP and go to the kernel stack.

The new canonical patch `0005-af-xdp-queue-and-tx-safety.patch` implements a deliberately fail-closed policy: it supports queue 0 only on an interface reporting exactly one RX queue, and checks the count before attach. It also adds XDP action counters for redirects and `XDP_PASS` on unmapped queues. It does **not** claim to implement multiqueue AF_XDP support.

**Still unproven:** the queue index selected for the actual failed SYN. Prior attempts did not obtain a packet capture or queue-specific trace. The single-queue fail-closed policy is intended to make the test configuration valid rather than allow silent packet bypass.

### B. TX-ring reservation bug

**Confirmed source defect.** The earlier `afxdp_send_pkts()` ignored the return from `xsk_ring_prod__reserve()`, then wrote/submitted the requested descriptors. The pinned libbpf contract is all-or-zero for the requested batch; reservation failure returns zero.

Patch `0005` changes the TX handling so a failed reservation does not write or submit descriptors and pending UMEM address/length information remains available for retry. Submitted frames stay outstanding until completion entries are consumed; reservation failures and TX/completion counters are exposed.

A focused test in `tests/afxdp_packet_io.c` exercises supported/unsupported queue counts and TX ring exhaustion/retry without writing/submitting unreserved slots.

**Still unproven:** no live handshake or high-pressure real AF_XDP TX run has been completed after this patch.

### C. XDP attachment left behind after termination

**Confirmed operational problem.** A bounded/forced smoke run left an XDP program attached. A subsequent run failed with `EBUSY` because the attach path uses no-replace semantics (`XDP_FLAGS_UPDATE_IF_NOEXIST`).

At audit time, `ip -details link show vxdp0` showed XDP program ID `582`. `bpftool` privileged inspection failed because non-interactive sudo required a password. Program identity/owner/mode could not be verified, and the agent correctly did not detach it.

The previous code's cleanup occurs on normal teardown, but SIGTERM/forced termination while blocked in `mtcp_accept()` can bypass the normal cleanup chain. An ownership-aware lifecycle patch (`0004-af-xdp-owned-lifecycle.patch`) already exists. Latest agent work also started improving shutdown signal handling in the smoke executable: pipe first, install handler after pipe exists, and block shutdown signals during mTCP initialization. The most recent log does not show a final test proving those changes work, especially while `mtcp_accept()` is blocked.

**Do not detach or reconfigure `vxdp0` or program 582.** Use a fresh, separate test interface for next runtime tests. The next agent may inspect `vxdp0` read-only, but must not assume ID 582 remains present without rechecking.

### D. Smoke test can provide false success

**Confirmed harness weakness.** The smoke path prints `Listening` before `mtcp_accept()` returns. A script had a child failure message but the child exit status was not propagated reliably; therefore exit code 0 did not prove success. The smoke program's own counter was based on number of parsed messages and bytes accepted by `mtcp_write()`, not peer receipt and response validation.

The test needs to pass only when the remote client receives and validates valid `CONNECT_ACK` and `PONG`. Child failures must propagate to the main process. `Listening` proves only listener setup, not TCP acceptance, packet RX, or protocol response.

### E. `mtcp_accept()` / blocked shutdown and runtime progression

`mtcp_accept()` is called before the smoke program enters its connection PING/PONG loop. It can block while the accept queue is empty. Until it returns, no Velora parser or PING/PONG code runs.

Latest agent work introduced a signal/pipe shutdown approach and blocked shutdown signals during initialization. Verify that the accept wait can actually observe the shutdown request and return. Installing a signal handler alone does not solve a blocking call if the wakeup path is not connected to that wait.

### F. EOF before queued output flush

Audit found that `mtcp_recv()` returning 0 currently causes `process_connection_data()` to signal closure; the event handler can close the connection before processing its later `EPOLLOUT` branch. A client that half-closes immediately after sending a request could lose the queued response. This is not a strong explanation for a client that waits for its response before shutdown, but it deserves a test/fix after the handshake works.

### G. Interface-name matching uses substring search

The pinned mTCP `SetNetEnv()`/interface matching path uses `strstr()` over the configured device-name string. This previously caused the test interface name `velora0` to match `lo` as a substring. Renaming to `vxdp0` bypassed that particular collision, but the general source defect remains. Fix exact-name matching through the canonical dependency patch process when practical. It does not appear to explain the current `port = vxdp0` selection.

### H. Configuration and environment

- mTCP `io = afxdp` is required for the intended path.
- The smoke config is intended to use one configured interface and one context/core.
- `config/route.conf` has zero extra routes for the isolated directly connected `/30` test.
- At the last inspected state, `config/arp.conf` had one entry for peer `172.31.254.2`; the previous invalid `ARP_ENTRY 0` and comment-only version were no longer the current contents.
- `enp3s0` was down/no-carrier and had no IPv4 address; it is not the interface for current testing.
- `wlan0` is active and must not be touched.
- An earlier veth pair had `vxdp0=172.31.254.1/30` and `vxdp1=172.31.254.2/30` in namespace `velora-peer`.
- The currently implemented patch deliberately rejects the four-RX-queue `vxdp0` before attaching XDP.

Relative paths must be tested from the documented working directory, and startup logging should say exactly which interface/config/BPF object was selected/opened.

### I. AF_XDP mode and zero-copy

The effective attach/bind mode was not verified. A successful attach/listening message must not be interpreted as proof of driver mode or zero-copy. Record the actual XDP attach flags, AF_XDP bind flags and mode accepted by the kernel for the test interface. No zero-copy claim is currently substantiated.

### J. Normal native build vs mTCP build

`debug`/`release` omit `src/net/mtcp_runtime.c`; `mtcp-runtime-smoke` compiles a dedicated executable with the mTCP runtime and pinned library. Native baseline benchmarks are useful control runs but are not results for the AF_XDP datapath.

---

## 5. Existing patch stack and latest known changes

Canonical patch series, in order:

1. `0001-clang-remove-obsolete-c-flag.patch`
2. `0002-xdp-ownership-and-failure-propagation.patch`
3. `0003-af-xdp-socket-map-registration.patch`
4. `0004-af-xdp-owned-lifecycle.patch`
5. `0005-af-xdp-queue-and-tx-safety.patch` (new)

The build script must apply the series using the exact flags that succeed against a fresh archive of the pinned dependency. The tests indicate that whitespace/recount-tolerant application options are needed for parts of the historical patch stack and the new patch. Do not “repair” a patch by editing a temporary tree that already contains a partial/rejected application; make a fresh temp archive for every full-series validation.

Latest agent excerpt reports a series of failed patch-generation attempts caused by reuse of disposable temp trees with rejected/partially applied changes. Those failures affected temporary `/tmp/velora-afxdp-*` working trees, not the canonical dependency checkout. The agent then created fresh trees such as `/tmp/velora-afxdp-seriesfresh.1Wnp44` and `/tmp/velora-afxdp-applyclean.*` and regenerated patch hunks. The last excerpt ends with “Made changes” but does not include the final `git status`, final diffs, or a complete post-signal-handler test summary.

Before continuing, verify whether the latest lifecycle/signal-handler changes modified `tests/mtcp_runtime_smoke.c`, `src/net/mtcp_runtime.c`, `0005`, or any other files. Do not assume that only five files are currently changed.

---

## 6. Validation already reported (must be rerun after latest edits)

The prior agent reported these passes **before the most recent signal/shutdown and patch-regeneration work was fully summarized**:

- Full canonical patch series applied to a fresh temporary archive of the pinned mTCP source: PASS.
- `make test-af-xdp-packet-io MTCP_SOURCE=/tmp/velora-afxdp-final.34STCb`: PASS.
- Existing protocol framing regression, compiled to a new temporary executable: PASS.
- Existing ring/connection regression, compiled to a new temporary executable: PASS.
- Patched `afxdp_module.c` syntax-only compilation: PASS, with legacy warnings.
- Modified BPF object build using clang/llc: PASS.
- `bash -n dependencies/build-mtcp-af_xdp.sh`: PASS.
- `git diff --check`: PASS.
- No pre-existing build binaries overwritten and no new live AF_XDP server started during that patch work.

Syntax checks still had inherited warnings including unused parameters, signedness, and mTCP's `mtcp_read` inline declaration. Do not silence them globally; determine whether each warning is relevant before making a narrow fix.

**No live TCP handshake, CONNECT/CONNECT_ACK, PING/PONG, graceful-shutdown lifecycle test or AF_XDP performance benchmark has been proven successful.** Re-run all validation against the current patch set because the latest agent then changed signal installation/shutdown logic and adjusted `0005` diagnostic placement.

---

## 7. Current live-host safety constraints

Last observed live state from the audit:

- `vxdp0` existed, had `172.31.254.1/30`, exposed four RX queues, and showed an XDP attachment, ID `582`.
- `velora-peer` namespace and `vxdp1` were part of the previous temporary topology.
- Privileged `bpftool` inspection failed because `sudo -n` required a password; attachment ownership and program details were not confirmed.
- No smoke server was running at that check.
- `enp3s0` had no carrier/IPv4 and is not to be touched; `wlan0` must not be touched.

This state may have changed since the log was collected. Recheck read-only before any network action. Do not detach ID 582 blindly and do not use a blanket `ip link ... xdp off` command.

The new patch is intentionally fail-closed on interfaces with more than one RX queue, so attempting the runtime on the old `vxdp0` will be rejected. The next live test should use a **fresh temporary veth pair and namespace**, created specifically for the test, and must verify the effective RX queue count before any XDP attach attempt. If a single-RX-queue veth cannot be configured and verified on this kernel, stop and report that blocker instead of weakening the fail-closed guard.

Do not persistently change memlock limits. Where needed, use the approved per-process limit invocation (for example `sudo prlimit --memlock=unlimited:unlimited -- ...`) on the exact smoke process.

---

## 8. Recommended order of future work

### Gate 0 — Reconcile current worktree

1. Capture root `git status`, diff and staged diff.
2. Capture canonical dependency patch list and build script diff.
3. Inspect the latest signal handler implementation and verify it does not call unsafe functions from a signal handler.
4. Check all patch files in a fresh extracted archive, using the exact flags in the canonical build script.
5. Re-run existing tests using separate temporary binaries/outputs.

### Gate 1 — Shutdown/lifecycle tests

1. Confirm the pipe exists before a handler can write to it.
2. Ensure signals blocked during initialization are unblocked at the correct point.
3. Prove the pipe or other safe mechanism wakes the runtime while `mtcp_accept()` is blocked. If not, add an explicit nonblocking/polling or interruptible accept strategy supported by the pinned mTCP API; do not assume the signal handler alone is enough.
4. Test graceful SIGINT and SIGTERM at startup, while listening, after accept, and during active request-response.
5. Verify owned XDP attachment, XSK, UMEM and context resources are removed after normal shutdown and after each partial-initialization failure.
6. Verify repeated start/stop does not leave a stale attachment or produce `EBUSY`.

### Gate 2 — Fresh one-RX-queue test topology

1. Re-inspect the existing state but leave `vxdp0` and its unknown attachment untouched.
2. Create a newly named veth pair and namespace under test ownership.
3. Configure addresses on a dedicated non-overlapping subnet and verify routes, peer MAC, `route.conf`, `arp.conf`, and cwd.
4. Verify the host-side interface has exactly one RX queue, for example via `/sys/class/net/<test-interface>/queues/rx-*` plus `ip -details link` or supported `ethtool` output.
5. Confirm no XDP program is attached to that new interface before starting the runtime.
6. Ensure the smoke configuration's `port` exactly matches the new interface name.

Do not reconfigure an existing interface if it cannot be shown to be test-owned.

### Gate 3 — Prove packet path

1. Start bounded smoke server with per-process memlock allowance.
2. While it is live, verify program identity, XSKMAP key 0 and live socket registration.
3. Capture packets on both test veth endpoints.
4. Connect from the peer and observe SYN, SYN-ACK and ACK.
5. Record whether `mtcp_accept()` returns.
6. If failed, identify the earliest missing transition; do not jump immediately to application protocol code.
7. Use XDP action counters and AF_XDP RX/TX/completion/reservation diagnostics. Aggregate packet counters are not enough to claim a queue-specific root cause.

### Gate 4 — Correct integration test semantics

The smoke test must fail nonzero if any of these are missing:

- TCP handshake and accept.
- Valid CONNECT received and CONNECT_ACK observed by the peer.
- Valid PING received and PONG observed by the peer.
- Child client failure propagated to the parent harness.
- Successful graceful cleanup and no owned XDP/XSK resources left behind.

`mtcp_write()` accepting bytes is not proof those bytes reached the peer. Validate the actual response frame on the client side.

Also test EOF/half-close when response output is queued so connection cleanup does not discard pending responses.

### Gate 5 — Benchmark

Only after Gate 4 passes:

- Use the same Velora protocol workload for native and mTCP + AF_XDP.
- Match concurrency, pipeline depth, payload sizes, warm-up, duration and client behavior.
- Run repeated trials; record per-trial results and variance.
- Report requests/sec, median and p95/p99 latency where sample count supports it, CPU utilization/cycles if available, failed connections, failed requests, retransmissions, timeouts and incomplete/in-flight requests.
- Verify and record actual XDP attach mode and AF_XDP copy/zero-copy mode; never infer these from “listening” or program attachment alone.
- Treat the native baseline as a control, not as an AF_XDP result.

---

## 9. Suggested task division for multiple agents

Avoid concurrent edits to the same patch files.

1. **Patch integration/reviewer:** reconcile `0001`–`0005` and script ordering using a fresh dependency archive; no network mutations.
2. **Lifecycle agent:** owns smoke signal/wakeup handling, partial-init cleanup and lifecycle tests. Coordinate any edits to `0004`/`0005` with the reviewer.
3. **Test-topology/integration agent:** after lifecycle changes are validated, creates a new isolated single-RX-queue veth test environment and gathers packet evidence. Must not touch `vxdp0`, `enp3s0`, `wlan0`, default route or firewall.
4. **Protocol/harness agent:** strengthens peer-validated CONNECT_ACK/PONG tests and exit-status propagation. Prefer independent test/harness files; do not modify AF_XDP patch internals.
5. **Benchmark agent:** wait until the complete acceptance path passes; then build paired reproducible benchmarks.

If only one agent is available, do the work sequentially in the same gate order.

---

## 10. Ready-to-paste prompt for the next agent

> Resume from `VELORA_AF_XDP_HANDOFF.md`. First inspect the current repository/worktree; do not assume the file list or status captured here is still exact. Preserve all edits, never reset/clean/stash, and do not modify ignored `.deps` as the only source of a fix. Reconcile and validate the canonical `0001`–`0005` patch series against a fresh archive of the pinned dependency using the exact build-script flags. The latest work introduced a smoke signal/pipe shutdown change and moved an AF_XDP startup diagnostic to after XSKMAP registration, but the supplied log does not prove these latest edits passed tests. Review them first. Then prove that SIGINT/SIGTERM can wake a runtime blocked in `mtcp_accept()` and that owned XDP/AF_XDP resources are cleaned on normal shutdown and partial-init failure. Do not run against the old `vxdp0`: it previously had four RX queues and XDP ID 582 whose ownership was not verified. Create a new isolated veth pair/namespace and verify exactly one RX queue before attaching. Capture SYN/SYN-ACK/ACK, confirm `mtcp_accept()`, then verify peer-received CONNECT_ACK and PONG. Stop at the exact first failure if any gate fails. Do not benchmark until the complete end-to-end test passes. Preserve `enp3s0`, `wlan0`, routes and firewall.

---

## 11. Definition of done

The integration is not “done” until all of these statements are backed by repeatable evidence:

- [ ] The canonical patch series applies to a fresh pinned dependency archive.
- [ ] AF_XDP queue-policy and TX-ring unit tests pass.
- [ ] Existing protocol and connection/ring tests pass.
- [ ] AF_XDP module syntax and BPF object build pass.
- [ ] Graceful shutdown and partial-initialization cleanup pass, including repeated start/stop.
- [ ] The test interface has exactly one RX queue and no unrelated XDP program before startup.
- [ ] XSKMAP registration is visible while the server is live.
- [ ] A packet capture demonstrates a completed TCP handshake.
- [ ] The mTCP runtime accepts the connection.
- [ ] The peer receives and validates CONNECT_ACK.
- [ ] The peer receives and validates PONG.
- [ ] The smoke harness reliably exits nonzero on any missing milestone.
- [ ] Shutdown leaves no process or resources owned by the test.
- [ ] Only after all above pass: matched native vs AF_XDP benchmark results are collected and reported.

**Current stop point:** the queue/TX patch had passed isolated unit/build checks, but the subsequent smoke shutdown/patch-regeneration work needs a fresh status and full validation. The live test is blocked until a safe single-RX-queue interface is prepared; no successful handshake/protocol exchange or AF_XDP benchmark has been established.

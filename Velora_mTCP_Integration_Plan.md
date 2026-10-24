# Velora + mTCP Integration Plan

**Purpose:** complete Velora integration with mTCP as quickly as practical while treating mTCP as a frozen external dependency. This document is not committed to the repository; it is the working execution plan.

**Repository snapshot used:** Vishisht-Dwivedi/Velora, branch af_xdp, remote HEAD 80a13595005444b04ce52206c655b010c6ab03bc (2026-10-24).

## Executive decision

Use the existing Velora-owned adapter in `src/net/mtcp_runtime.c` and build a **separate `build/velora-mtcp` server executable**. Keep the native `build/velora` entry point and reactor unchanged. The separate executable should reuse Velora connection buffers, parser, and protocol handler while mTCP provides sockets, TCP, event delivery, and the AF_XDP-backed I/O module. This is the shortest path to an end-to-end result without prematurely refactoring Velora into a selectable multi-backend architecture.

## Non-negotiable dependency boundary

- Do not edit any mTCP C source, headers, or library internals.
- Do not edit `dependencies/patches/`, `dependencies/build-mtcp-af_xdp.sh`, or `dependencies/mtcp-af_xdp.lock` as part of this integration pass.
- Do not alter or clean the ignored `.deps/mtcp-af_xdp/source/` checkout.
- Prefer the existing, already-built mTCP dependency artifacts. Do not invoke `make mtcp-af_xdp` during this plan because the current script applies patches to a fresh mTCP checkout.
- **Important constraint conflict:** the current repository build script applies existing patches 0001-0005 to a fresh mTCP checkout. If no usable prebuilt dependency exists without applying those patches, report the conflict and stop rather than silently violating the no-mTCP-changes rule. A pristine, reproducible dependency build is a separate decision; it must not be smuggled into integration tasks.

## Current-stage facts from source

- `src/net/mtcp_runtime.c` already adapts the mTCP socket/epoll API to Velora connection management, parser, protocol, and ring buffers.
- The root Makefile deliberately excludes `src/net/mtcp_runtime.c` from the normal server build; `src/main.c` still launches the native epoll server.
- `tests/mtcp_runtime_smoke.c` currently serves only one accepted connection and calls success based on packet/byte counters. Those counters do not prove the peer received `CONNECT_ACK` and `PONG`.
- `vr_mtcp_runtime_accept_one()` already accepts and registers a client. A new main loop can call it repeatedly and then call `vr_mtcp_runtime_process_events()` to serve multiple clients without changing mTCP.
- The current `STREAM_CLOSE` handler is one-way, but its accounting can underflow on invalid or duplicate closes. The live scripts should not wait for a response that the implementation does not define.
- AF_XDP runtime success must be proven on a dedicated, correctly configured test interface. A compile, XDP attach, “Listening” log, or mTCP write acceptance is not sufficient evidence.

## Target architecture

```text
native path:  src/main.c -> native epoll reactor -> kernel TCP sockets (unchanged)

mTCP path:    src/mtcp_main.c (new)
                 -> src/net/mtcp_runtime.c (Velora adapter)
                 -> mTCP public socket + epoll API
                 -> mTCP AF_XDP I/O backend (frozen dependency)
                 -> Velora ring buffer / parser / protocol handler

test path:    black-box client checks CONNECT_ACK and PONG on the peer
```

Do not first refactor the native reactor into a generic transport abstraction. A separate executable gives a smaller diff and makes failures attributable to the adapter. Backend selection inside a single `main` can be a later cleanup after the AF_XDP path is proven.

## Definition of done

1. `build/velora` still builds and behaves as the native backend.
2. `build/velora-mtcp` builds against the existing mTCP artifact without edits to mTCP source or dependency patching.
3. The mTCP server repeatedly accepts clients and uses Velora parsing/protocol code.
4. A black-box client validates actual `CONNECT_ACK` and `PONG` bytes; timeouts are failures.
5. A live test on the intended dedicated AF_XDP interface completes TCP handshake, Velora CONNECT/CONNECT_ACK, PING/PONG, and safe shutdown.
6. Native and mTCP protocol/buffer tests pass; multi-client tests pass at bounded sizes.
7. Any performance comparison counts server-confirmed transactions and uses equivalent workload/configuration.
8. Git diff confirms no new or altered mTCP source/patch/build files and no unrelated user changes were discarded.

## Execution protocol

Copy one prompt at a time, in order. Wait for that agent to return its exact changes and tests. Review the diff before sending the next prompt. Every prompt has a narrow scope and acceptance criterion; if a prompt discovers a dependency-side blocker, stop there rather than modifying mTCP.

## P00 — Read-only preflight and dependency immutability check

**Phase:** Phase 0 — Lock the baseline  
**Scope:** No edits. Do not run the dependency build script.

**Copy-paste prompt**

```text
You are working in the Velora repository. First establish the exact working state before doing any implementation.

Hard constraints:
- Do not modify mTCP source, including anything under .deps/mtcp-af_xdp/source/.
- Do not modify dependencies/patches/, dependencies/build-mtcp-af_xdp.sh, or dependencies/mtcp-af_xdp.lock.
- Do not run make mtcp-af_xdp or any command that applies patches to mTCP.
- Do not reset, clean, stash, overwrite build outputs, or discard user changes.
- Treat Markdown status documents as unverified; inspect code and Git state directly.

Run and report: git status --short --branch --untracked-files=all; git rev-parse HEAD; git log -8 --oneline --decorate; git diff --stat; git diff --cached --stat. Check whether the existing .deps/mtcp-af_xdp/source checkout and required built artifacts (libmtcp.a, libbpf.a, afxdp_kern.o, public headers) exist. If the dependency checkout exists, report its HEAD and status without changing it. Inspect the Makefile and dependency script only to explain which commands would modify the dependency.

Return a concise baseline: exact branch/commit, relevant local edits, available artifacts, and the safest existing build command that does NOT mutate mTCP. If a usable prebuilt dependency is unavailable without applying the existing patches, stop and report that as a blocker; do not bypass the constraint.
```

**Acceptance criterion:** A factual baseline is returned; no files are changed and no mTCP patch/build command is run.

## P01 — Add a peer-validating mTCP protocol client

**Phase:** Phase 1 — Make protocol success observable  
**Scope:** New test file only; no server or dependency edits.

**Copy-paste prompt**

```text
In Velora, add tests/mtcp_protocol_client.py as a small black-box TCP client for validating a running Velora mTCP server.

Hard constraints: do not modify mTCP source, .deps/mtcp-af_xdp/source/, dependencies/patches/, the dependency build script, or the dependency lock. Do not infer success from server logs or bytes accepted by mtcp_write().

The client must accept host and port arguments, use bounded socket timeouts, encode/decode the existing 8-byte Velora header (network byte order; magic 0x5789; version 1), and verify every response field relevant to the test. It must connect, send CONNECT, verify a valid CONNECT_ACK on stream 0 with zero flags and zero payload, then send PING and verify a valid PONG on stream 0 with zero flags and zero payload. Then test several pipelined PING packets and verify the same number of PONG packets. Add one fragmented-header delivery case. Read exactly the expected bytes; reject EOF, unexpected packet types, malformed envelopes, missing responses, and timeouts with a nonzero exit code. Do not wait for a response to PUBLISH or STREAM_CLOSE; the current handler treats them as one-way operations.

Keep dependencies to the Python standard library. Include clear PASS/FAIL output and a nonzero exit on any failed assertion. Do not add a server launcher in this prompt.
```

**Acceptance criterion:** A standalone client can prove the peer received the expected protocol response bytes and cannot pass on timeout.

## P02 — Add a separate mTCP server entry point

**Phase:** Phase 2 — Turn the adapter into a server  
**Scope:** New src/mtcp_main.c only, unless a compile-time interface requires one header addition.

**Copy-paste prompt**

```text
Implement a production-style entry point for Velora's existing mTCP runtime by adding src/mtcp_main.c. Keep the native server path in src/main.c unchanged.

Hard constraints: no edits to mTCP internals or dependency build/patch files. Reuse include/velora/mtcp_runtime.h and the functions already implemented in src/net/mtcp_runtime.c; do not reimplement packet parsing or the protocol state machine.

The executable must accept exactly: <mtcp-config-file> <bind-ipv4> <port>. Validate arguments and port. Initialize Velora logging, initialize vr_mtcp_runtime_t with VR_MTCP_RUNTIME_INITIALIZER, initialize the runtime, and listen. Keep serving until SIGINT/SIGTERM. In the main loop, repeatedly call vr_mtcp_runtime_accept_one() until no more accepts are pending, then call vr_mtcp_runtime_process_events() with a short bounded timeout. Handle errors without continuing in a corrupted state. Use a signal-safe shutdown flag/approach; do not call cleanup functions directly from a signal handler. Ensure normal shutdown calls vr_mtcp_runtime_destroy() and closes the logger. The server must continue accepting clients after the first connection and must not exit after serving one request.

Use the existing API as-is. If the existing runtime API prevents this without changing mTCP, change only Velora-owned runtime files and explain why in the final summary. Do not alter the mTCP API.
```

**Acceptance criterion:** A standalone source entry point runs an event loop, accepts multiple clients over time, handles SIGINT/SIGTERM, and cleans up through Velora APIs.

## P03 — Add a dedicated Makefile target

**Phase:** Phase 2 — Turn the adapter into a server  
**Scope:** Root Makefile only.

**Copy-paste prompt**

```text
Add a dedicated root Makefile target to build the new Velora mTCP server entry point as build/velora-mtcp.

Hard constraints: do not modify mTCP source or any dependency patch/build/lock files. Do not change the existing native debug/release source list or the default build output. Use the current MTCP_RUNTIME_SRCS, MTCP_CPPFLAGS, and MTCP_LDLIBS variables where appropriate, and compile src/mtcp_main.c with the existing Velora mTCP runtime and protocol/buffer sources. Add the target to .PHONY. Add helpful preflight checks for the existing mTCP headers/library so a missing dependency fails with an actionable message rather than a confusing compiler/linker error. Do not automatically invoke make mtcp-af_xdp because its current script applies patches to the dependency checkout.

Run the native release build and the new target if the required existing dependency artifacts are already available. If they are not available under the no-mTCP-modification rule, report that blocker and leave the target in a sensible, testable state; do not patch or rebuild mTCP to get around it.
```

**Acceptance criterion:** The native target remains unchanged; a new build/velora-mtcp target exists and is isolated from the native main/server.

## P04 — Add a safe launcher for the mTCP server

**Phase:** Phase 2 — Turn the adapter into a server  
**Scope:** New scripts/run-velora-mtcp.sh only.

**Copy-paste prompt**

```text
Add scripts/run-velora-mtcp.sh to launch the Velora mTCP server using the existing pinned dependency layout.

Hard constraints: do not modify mTCP source, configs, dependency patches, the dependency build script, or the lock file. Do not change host network interfaces or attach/detach XDP programs in this launcher.

The script must use set -euo pipefail, resolve the Velora repo root robustly, require an existing build/velora-mtcp executable, validate that the supplied mTCP config file exists, and accept <config-file> <bind-ipv4> <port>. It should change working directory to the pinned mTCP source's expected runtime directory before exec, because this mTCP revision resolves some config and BPF object paths relative to its current working directory. Convert the supplied config to an absolute path before changing directories. If the expected directory or executable is missing, fail with a clear diagnostic. Do not silently run a dependency build or apply patches. Keep signal delivery and exit status transparent by using exec where possible. Add brief usage comments in the script.
```

**Acceptance criterion:** Launching does not depend on the caller's current directory, does not mutate dependencies, and preserves the server's exit code.

## P05 — Add an integration smoke runner that checks the peer

**Phase:** Phase 3 — Verify request/response end to end  
**Scope:** New tests/mtcp_integration_smoke.sh only.

**Copy-paste prompt**

```text
Add tests/mtcp_integration_smoke.sh to run a bounded end-to-end smoke test against an already-built Velora mTCP server.

Hard constraints: do not modify mTCP source or dependency patch/build files. Do not create or alter network interfaces. The test must use a caller-provided mTCP config, bind IPv4 address, port, and client destination; it must never guess a NIC or interface.

The script should launch scripts/run-velora-mtcp.sh in the background, capture logs to a temporary file, install cleanup traps, wait with a timeout for a reliable server-ready indication, run tests/mtcp_protocol_client.py against the server, and stop the server gracefully. If the server dies early, fails to become ready, the client fails, or shutdown times out, print useful logs and exit nonzero. Never treat a timeout as a successful result. Avoid broad pkill commands; terminate only the PID started by this script. The script must not call make mtcp-af_xdp. Use only temporary files and remove them on exit unless a KEEP_LOGS option is set.
```

**Acceptance criterion:** One command performs a bounded peer-validated smoke test and cleans up only the process it started.

## P06 — Fix adapter defects only when a failing test proves them

**Phase:** Phase 3 — Verify request/response end to end  
**Scope:** Velora-owned src/net/mtcp_runtime.c and associated headers/tests only.

**Copy-paste prompt**

```text
Run the peer-validating mTCP protocol client against the new Velora mTCP server in an already-prepared, authorized test environment. If a test fails, diagnose the exact path using source and logs, then make the smallest necessary fix in Velora-owned code only.

Hard constraints: do not modify mTCP source, .deps/mtcp-af_xdp/source/, dependencies/patches/, dependencies/build-mtcp-af_xdp.sh, or dependencies/mtcp-af_xdp.lock. Do not claim AF_XDP works unless the client communicates through the configured mTCP AF_XDP backend. Do not make speculative refactors or tune performance yet.

Focus on the existing adapter path: nonblocking accept, connection registration, reads into the ring buffer, parser polling, protocol dispatch, response queueing, partial mtcp_write handling, write-interest toggling, and connection cleanup. Preserve TCP byte-stream semantics. Add/adjust the narrowest regression test for each discovered defect. At the end, provide the exact failing behavior, root cause, changed Velora files, and test command/output. If the issue can only be fixed by modifying mTCP, do not make that change; report it as an explicit blocker.
```

**Acceptance criterion:** Only evidence-backed Velora-side fixes are made; protocol-client tests pass or a precise mTCP-side blocker is documented.

## P07 — Make STREAM_CLOSE accounting safe

**Phase:** Phase 3 — Verify request/response end to end  
**Scope:** Velora protocol implementation and protocol tests only.

**Copy-paste prompt**

```text
Fix the stream lifecycle bug in Velora's src/protocol/protocol.c without changing the current wire contract. The current implementation removes a stream bit and decrements active_streams without checking whether the stream is open; repeated closes, closing an unopened stream, or closing the default stream can corrupt accounting.

Hard constraints: no mTCP source or dependency changes. Keep STREAM_CLOSE one-way for this integration pass: do not invent an acknowledgement packet. Reject or safely ignore invalid close attempts according to the existing protocol's error-handling conventions, but never underflow active_streams, clear an unallocated bit, or close the default stream accidentally. Ensure closing a valid non-default stream changes the bitmap and counter exactly once. Add focused tests for valid close, duplicate close, unopened stream, stream 0, and close when active_streams is one. Run the existing protocol test target.
```

**Acceptance criterion:** Stream accounting remains valid for malformed/duplicate close requests; no new STREAM_CLOSE wire response is invented.

## P08 — Make integration tests easy to invoke

**Phase:** Phase 4 — Regression and repeatability  
**Scope:** Root Makefile and Velora-owned test files only.

**Copy-paste prompt**

```text
Add focused Makefile targets for the Velora mTCP protocol client checks and integration smoke runner, without changing the native targets.

Hard constraints: do not modify mTCP source, patch files, dependency build script, or lock file. Do not make a test that needs root or changes interfaces run implicitly as part of make all, debug, or release.

Provide a lightweight target for the Python protocol client's self-checks if you add any, and a separately named opt-in integration target that requires explicit config/bind/port/client arguments (or clearly documented environment variables). Keep privileged/live AF_XDP tests opt-in. Run make test-protocol, make test-buffers, make release, and the new target(s) that can be tested without a live NIC. Ensure each target propagates failures as nonzero exit status. Report exactly which tests were executed and which were not.
```

**Acceptance criterion:** Native regressions and unprivileged tests are separate from explicit live mTCP/AF_XDP tests; failures cannot be swallowed.

## P09 — Add a dependency immutability check to the final audit

**Phase:** Phase 4 — Regression and repeatability  
**Scope:** Read-only inspection; do not edit dependency tooling.

**Copy-paste prompt**

```text
Audit the integration diff against the no-mTCP-change rule. Do not edit anything in this prompt.

Run git status --short --branch --untracked-files=all, git diff --stat, and git diff --cached --stat. Verify that there are no edits to .deps/mtcp-af_xdp/source/, dependencies/patches/, dependencies/build-mtcp-af_xdp.sh, or dependencies/mtcp-af_xdp.lock. If the ignored dependency checkout exists, inspect its status and compare it with the baseline captured in P00 without resetting or cleaning it. Report whether any build/test command unexpectedly modified dependency sources.

Important: the current checked-in dependency script applies existing patches 0001-0005 when it builds a fresh mTCP checkout. Do not invoke that script or change the patch series. If no prebuilt dependency is available without applying those patches, identify the conflict between a clean reproducible rebuild and the strict no-mTCP-modification constraint; do not hide it or choose for the user.
```

**Acceptance criterion:** A factual immutability report exists and no dependency reset, patch, or source modification was performed.

## P10 — Run the smoke test on an isolated test interface

**Phase:** Phase 5 — Live AF_XDP smoke  
**Scope:** Execution-only; use an already-configured dedicated interface/veth pair.

**Copy-paste prompt**

```text
Run the already-built Velora mTCP integration smoke test using a dedicated, preconfigured test interface or veth pair and an mTCP config supplied by the operator.

Hard constraints: do not modify mTCP source or dependency patches. Do not configure, down, detach XDP from, or otherwise alter the host's normal network interface. Do not create veth pairs or change routes unless the operator explicitly asked for that setup in this same task. Do not run on a production NIC. Do not infer success from “Listening”, successful XDP attach, or counters alone.

Record the exact config path, interface, bind/client addresses, port, kernel, attach mode, and commands. Run the peer-validating client and require CONNECT_ACK and PONG validation. Capture server stdout/stderr and the client exit code. If setup fails, report the first failing stage (runtime init, XDP attach, UMEM/socket, XSKMAP, TCP handshake, application CONNECT, response delivery, or teardown) using available evidence. Do not change mTCP to make the run pass.
```

**Acceptance criterion:** An isolated, peer-validated live run is recorded—or the failure is localized without touching a normal NIC or mTCP source.

## P11 — Verify graceful shutdown and owned resource cleanup

**Phase:** Phase 5 — Live AF_XDP smoke  
**Scope:** Velora-owned entry point/scripts/tests; never force-detach an unknown XDP program.

**Copy-paste prompt**

```text
Validate the shutdown path of the Velora mTCP server using the dedicated test interface from the successful or attempted P10 run.

Hard constraints: do not modify mTCP source/dependency patches. Never detach an XDP program unless its identity is verified as the program owned by this specific test. Never kill unrelated processes. Do not use SIGKILL as the normal shutdown path.

Run the server, perform a valid CONNECT/PING/PONG exchange, send SIGTERM to the exact server PID, wait for exit, and record the status and logs. Verify only resources that can be safely identified: the server process exited, the test client sees the expected closure, and any XDP program/map state is checked by program ID/ownership rather than blindly detached. Repeat start/stop if safe. If the current third-party backend exits the process during partial initialization or leaks resources in a way Velora cannot control through public APIs, document the exact limitation; do not patch mTCP.
```

**Acceptance criterion:** Graceful shutdown behavior is documented with ownership-safe checks; no unrelated interface program is detached.

## P12 — Exercise multiple clients and fragmented streams

**Phase:** Phase 6 — Concurrency and resilience  
**Scope:** Velora-owned tests only.

**Copy-paste prompt**

```text
Extend tests/mtcp_protocol_client.py (or add a companion test) to validate multiple simultaneous client sessions against the Velora mTCP server. Keep the workload bounded and configurable.

Hard constraints: no mTCP source/dependency changes; no unbounded stress test and no default 10,000-client run. Each client must independently complete CONNECT/CONNECT_ACK and PING/PONG. Include fragmented headers and several pipelined PINGs. Verify exact response counts and envelopes. Make timeouts fail, close all sockets in finally/cleanup paths, and print requested versus completed sessions. Start with 2, 10, and 100 clients if the configured environment supports them; do not claim support for larger counts without results.

Keep protocol correctness tests separate from throughput tests. If failure correlates with connection lookup, event batching, or write interest, capture a minimal reproduction before changing code. Do not perform algorithmic optimization in this prompt.
```

**Acceptance criterion:** Bounded multi-client tests establish correctness and provide exact counts/errors, not just successful socket connects.

## P13 — Run an apples-to-apples native versus mTCP baseline

**Phase:** Phase 7 — Measure only after correctness  
**Scope:** Velora-owned benchmark runner/results only.

**Copy-paste prompt**

```text
Once P10's peer-validated mTCP path passes, prepare a fair comparison between the native Velora server and the mTCP/AF_XDP server using the same client workload and protocol semantics.

Hard constraints: no mTCP source/dependency changes. Do not run the mTCP case unless P10 passed. Do not compare the existing tests/throughput.sh send-acceptance metric against a server-confirmed transaction metric. Do not make performance claims from client send() acceptance alone.

Use the same binary client, connection count, pipeline depth, payload, warmup, measurement duration, and repeated runs for both backends. Count throughput only when a valid PONG is received. Record p50/p95/p99 latency, errors, successful connections, server CPU, host CPU, kernel, CPU model, backend/attach mode, configuration, and exact commit. Report medians and spread, include the native baseline and mTCP results, and do not claim a speedup if the tested network paths or workload differ.
```

**Acceptance criterion:** A reproducible paired result exists, or the report clearly says that a fair benchmark could not yet be run.

## P14 — Final code, policy, and test audit

**Phase:** Phase 8 — Close the integration task  
**Scope:** Read-only audit plus final report; do not add scope.

**Copy-paste prompt**

```text
Perform a final review of the Velora mTCP integration changes. Do not add new features or do unrelated refactoring.

Hard constraints: no mTCP source or dependency patch/build/lock changes; no destructive Git commands. Review the complete first-party diff, not Markdown status files. Verify that the native build path remains unchanged, build/velora-mtcp is separate, the server continues accepting clients, response success is peer-validated, tests fail on timeout, and live tests are opt-in. Re-run all applicable unprivileged tests and the known-good live smoke test if the environment remains available.

Return a concise status with four explicit categories: implemented, tested and passed (with exact commands), not tested, and blockers/known limitations. Include current branch/commit and git status. Confirm whether the no-mTCP-change rule was respected. Do not label the integration complete unless the real AF_XDP-configured server completes TCP handshake + Velora CONNECT/CONNECT_ACK + PING/PONG + safe shutdown on the intended test path.
```

**Acceptance criterion:** The final status distinguishes source implementation, executed evidence, and unresolved blockers.

## Work deliberately deferred

- Unifying the native and mTCP server entry points into one configurable backend.
- Rewriting mTCP internals or adding new dependency patches.
- Optimizing the adapter’s linear connection lookup until correctness and baseline performance are known.
- Zero-copy packet views and transport-level architectural redesign.
- Broad stress runs or any changes to a normal host NIC.

These are intentionally deferred to keep the first integration milestone small and measurable.
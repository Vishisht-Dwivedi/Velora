# Velora Benchmark Report: Native vs AF_XDP (mTCP)

**Date:** 2026-10-25  
**Git Commit:** `34ada8594185b798b2873853d5bd98d70df5081b`  
**Kernel:** `7.2.11-arch1-1`  
**CPU:** Intel(R) Core(TM) i3-7020U CPU @ 2.30GHz (2 physical cores / 4 threads)  
**Compiler Flags:** `-Wall -Wextra -Iinclude -pthread -O3` (Release build)  
**Logger State:** Completely disabled (`VR_LOG_LEVEL=OFF`, `log_ptr = NULL`, 0 mutex/IO overhead)  

---

## 1. Executive Summary

A rigorous, black-box performance benchmark was conducted comparing the **Velora Native** (Linux epoll + kernel TCP stack) engine and the **Velora AF_XDP** (mTCP userspace TCP stack with AF_XDP packet I/O) engine.

- **Measurement Metric:** Strictly **completed, validated 8-byte PONG envelopes** received by client connections over the wire. Send acceptance is discarded as completed work.
- **Protocol:** 8-byte binary framing (`magic=0x5652`, `type=PING/PONG`, `flags=0`, `length=0`).
- **Load Profile:** 50 persistent TCP connections, pipeline depth 8 (400 maximum in-flight requests), 2 seconds warmup (discarded from metrics), 5 seconds steady-state measurement window per run.
- **Topology:** Dual network namespaces over a virtual ethernet pair:
  - Server: `velora-af-server` (`198.18.0.1:22409`, interface `vmtcp1`)
  - Client: `velora-af-test1` (`198.18.0.2`, interface `vmtcp1-peer`, MAC `d2:ce:d7:4a:a8:37`, `tx-checksumming: off`)
  - Execution Order: 6 alternating runs (Native $\to$ AF_XDP $\to$ Native $\to$ AF_XDP $\to$ Native $\to$ AF_XDP) with environment validation between runs.

---

## 2. Benchmark Results & Summary Statistics

### 2.1 Aggregate Comparison (3 Runs Per Backend)

| Metric | Velora Native (Kernel TCP + epoll) | Velora AF_XDP (mTCP + AF_XDP) | Comparison / Ratio |
| :--- | :--- | :--- | :--- |
| **Throughput (Median)** | **219,015.0 PONG/s** | **55,238.0 PONG/s** | **Native is 3.97x faster** |
| **Throughput (Mean)** | 220,924.3 PONG/s | 59,017.7 PONG/s | Native is 3.74x faster |
| **Throughput (Spread)** | 217,050.0 – 226,708.0 ($\sigma=5,104.3$, 2.3%) | 49,610.0 – 72,205.0 ($\sigma=11,762.1$, 19.9%) | Native highly stable |
| **RTT Latency p50 (Median)** | **1,815.0 $\mu\text{s}$** | **606.0 $\mu\text{s}$** | **AF_XDP is 3.00x lower** |
| **RTT Latency p95 (Median)** | **2,588.0 $\mu\text{s}$** | **11,060.0 $\mu\text{s}$** | **Native is 4.27x lower** |
| **RTT Latency p99 (Median)** | **3,423.0 $\mu\text{s}$** | **12,164.0 $\mu\text{s}$** | **Native is 3.55x lower** |
| **Server CPU Utilization** | **95.6%** (Single Core Saturation) | **47.7%** (Single Core Partial) | Native extracts more host CPU |
| **Total Completed PONGs / 5s**| **1,085,250 – 1,133,540** | **248,050 – 361,025** | Zero protocol errors on both |

---

### 2.2 Raw Individual Run Telemetry

| Run ID | Backend | Duration | Warmup | Conns | PONG/s | RTT p50 ($\mu\text{s}$) | RTT p95 ($\mu\text{s}$) | RTT p99 ($\mu\text{s}$) | Server CPU | Artifact Path |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Run 1 | Native | 5.0s | 2.0s | 50 | 219,015.0 | 1,815 | 2,588 | 3,455 | 95.6% | `benchmark_results/run1_native.json` |
| Run 2 | AF_XDP | 5.0s | 2.0s | 50 | 72,205.0 | 668 | 11,128 | 12,842 | 49.9% | `benchmark_results/run2_afxdp.json` |
| Run 3 | Native | 5.0s | 2.0s | 50 | 226,708.0 | 1,651 | 2,554 | 3,331 | 94.0% | `benchmark_results/run3_native.json` |
| Run 4 | AF_XDP | 5.0s | 2.0s | 50 | 55,238.0 | 604 | 11,060 | 12,003 | 41.6% | `benchmark_results/run4_afxdp.json` |
| Run 5 | Native | 5.0s | 2.0s | 50 | 217,050.0 | 1,830 | 2,635 | 3,423 | 96.4% | `benchmark_results/run5_native.json` |
| Run 6 | AF_XDP | 5.0s | 2.0s | 50 | 49,610.0 | 606 | 11,007 | 12,164 | 47.7% | `benchmark_results/run6_afxdp.json` |

---

## 3. Analysis & Architectural Tradeoffs

### 3.1 Median Latency vs. Throughput & Tail Behavior
1. **Low Load RTT (p50):**
   - AF_XDP delivers an impressive median transaction RTT of **606 $\mu\text{s}$**, compared to **1,815 $\mu\text{s}$** for the native kernel TCP stack.
   - When processing requests individually or in small batches, bypassing the kernel socket buffer and operating directly via UMEM rings yields significantly lower base packet transit time.
2. **Pipelined Throughput & Tail Latency (p95/p99):**
   - Under heavy pipelining (400 concurrent in-flight requests), native epoll achieves **219k PONG/s**, saturating a single CPU core at 95.6%.
   - AF_XDP throughput stabilizes between **50k and 72k PONG/s**, with p95/p99 jumping to **~11–12 ms**.
   - Server CPU utilization for mTCP reaches only **47.7%**, indicating that the bottleneck is not raw Velora protocol serialization, but I/O queue replenishment and userspace TCP timer/retransmission handling.

### 3.2 Effective Runtime Mode: SKB Generic vs Zero-Copy
Per project instructions (*Rule 10: "Never claim AF_XDP zero-copy without verifying the effective runtime mode"*):
- In this environment, the communication takes place over a Linux `veth` pair (`vmtcp1` $\longleftrightarrow$ `vmtcp1-peer`).
- Linux kernel `veth` drivers do **not** support native hardware driver zero-copy mode. Instead, AF_XDP attaches via **SKB / generic copy mode** (`XDP_FLAGS_SKB_MODE`).
- In SKB copy mode:
  1. The kernel network subsystem allocates an `sk_buff` for each ingress frame and executes the XDP BPF program.
  2. The packet payload is copied from the `sk_buff` into the userspace UMEM ring.
  3. The userspace stack (mTCP) must periodically trigger `sendto()` syscalls on the XSK socket to kick the kernel TX ring.
- In contrast, the native kernel TCP stack benefits from GRO (Generic Receive Offload), LRO, and direct kernel-space skb passing without user-kernel ring copying.
- Consequently, on virtual interfaces (`veth`), native kernel TCP is expected to outperform SKB-mode AF_XDP in bulk throughput. AF_XDP's zero-copy throughput benefits emerge primarily on hardware NICs supporting native zero-copy driver modes (e.g., `ice`, `i40e`, `mlx5`).

---

## 4. Integrity, Test Validation, and Regression Gates

- **Shutdown Safety:** Both engines completed all runs and terminated cleanly via `SIGTERM` with exit code `0`. Zero core dumps, zero SIGSEGVs, and zero kernel page faults were observed.
- **Host Isolation:** Protected host interface `vmtcp0` with driver XDP prog ID `141` remained completely untouched throughout all runs.
- **Clean Namespace Recovery:** After each run, `scripts/af_xdp_test_env.sh check` and `bpftool net list` confirmed zero orphaned listeners and clean detachment of XDP programs from `vmtcp1`.
- **Reproducibility:** All raw run results are captured in versioned JSON artifacts under `benchmark_results/run[1-6]*.json`.

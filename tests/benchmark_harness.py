#!/usr/bin/env python3
"""Unified benchmark harness for Velora Native vs AF_XDP backends.

Runs throughput_pingpong (black-box PING/PONG transaction benchmark)
across isolated network namespaces (velora-af-server and velora-af-test1),
recording validated PONG completions, throughput, percentiles, and CPU metrics.
"""

import argparse
import json
import os
import platform
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINGPONG_BIN = ROOT / "build" / "throughput_pingpong"
NATIVE_BIN = ROOT / "build" / "velora"
MTCP_BIN = ROOT / "build" / "velora-mtcp"
LAUNCHER = ROOT / "scripts" / "run-velora-mtcp.sh"
ENV_HELPER = ROOT / "scripts" / "af_xdp_test_env.sh"

PATTERNS = {
    "requested_connections": r"Requested connections\s*:\s*(\d+)",
    "established_connections": r"Established\s*:\s*(\d+)",
    "active_connections": r"Connections alive\s*:\s*(\d+)",
    "connection_failures": r"Connection failures\s*:\s*(\d+)",
    "registration_failures": r"Registration failures\s*:\s*(\d+)",
    "pipeline_depth": r"Pipeline depth\s*:\s*(\d+)",
    "completed_transactions": r"Completed PING/PONGs\s*:\s*(\d+)",
    "throughput_ops_per_second": r"Average throughput\s*:\s*([0-9.]+)",
    "latency_us": r"RTT p50/p95/p99\s*:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)",
    "latency_samples": r"RTT samples\s*:\s*(\d+)",
    "errors": r"Total errors\s*:\s*(\d+)",
    "in_flight_at_end": r"Requests in flight at end:\s*(\d+)",
    "measured_seconds": r"Benchmark runtime\s*:\s*([0-9.]+)",
    "warmup_seconds": r"Warmup duration\s*:\s*(\d+)",
}


def parse_client_output(output, expected_clients):
    """Parse output from throughput_pingpong and validate correctness."""
    result = {}
    for key, pattern in PATTERNS.items():
        match = re.search(pattern, output)
        if match is None:
            if key == "warmup_seconds":
                result[key] = 0
                continue
            raise ValueError(f"Benchmark output missing expected key: {key}")
        values = match.groups()
        if key == "latency_us":
            result[key] = {
                "p50": int(values[0]),
                "p95": int(values[1]),
                "p99": int(values[2]),
                "unit": "microseconds",
                "scope": "per_ping_rtt",
            }
        elif key in ("throughput_ops_per_second", "measured_seconds"):
            result[key] = float(values[0])
        else:
            result[key] = int(values[0])

    passed = "[RESULT] PASS -" in output
    healthy = (
        passed
        and result["requested_connections"] == expected_clients
        and result["established_connections"] == expected_clients
        and result["active_connections"] == expected_clients
        and result["connection_failures"] == 0
        and result["registration_failures"] == 0
        and result["errors"] == 0
    )
    if not healthy:
        raise RuntimeError(
            f"Benchmark correctness check failed. Output excerpt:\n{output[-3000:]}"
        )
    return result


def get_cpu_model():
    """Retrieve CPU model name."""
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def get_git_commit():
    """Retrieve current Git commit SHA."""
    try:
        out = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True)
        return out.strip()
    except (subprocess.CalledProcessError, OSError):
        return "unknown"


def process_cpu_seconds(pid):
    """Read process user + kernel cpu seconds from /proc/<pid>/stat."""
    try:
        stat_text = Path(f"/proc/{pid}/stat").read_text()
        fields = stat_text[stat_text.rfind(")") + 2 :].split()
        ticks = int(fields[11]) + int(fields[12])
        return ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, IndexError, ValueError):
        return 0.0


def system_cpu_counters():
    """Read per-core system CPU counters from /proc/stat."""
    counters = []
    try:
        for line in Path("/proc/stat").read_text().splitlines():
            fields = line.split()
            if not fields or not re.fullmatch(r"cpu\d+", fields[0]):
                continue
            values = [int(v) for v in fields[1:]]
            idle = values[3] + (values[4] if len(values) > 4 else 0)
            counters.append((fields[0], sum(values), idle))
    except (OSError, ValueError):
        return None
    return counters


def calculate_cpu_percent(before_counters, after_counters):
    """Calculate per-core CPU utilization percentage."""
    if not before_counters or not after_counters:
        return {}
    before_map = {name: (tot, idle) for name, tot, idle in before_counters}
    res = {}
    for name, tot_after, idle_after in after_counters:
        if name not in before_map:
            continue
        tot_before, idle_before = before_map[name]
        d_tot = tot_after - tot_before
        d_idle = idle_after - idle_before
        res[name] = round(100.0 * (d_tot - d_idle) / d_tot, 2) if d_tot > 0 else 0.0
    return res


def compute_aggregate_stats(runs):
    """Compute summary statistics across multiple benchmark runs."""
    if not runs:
        return {}
    throughputs = [r["throughput_ops_per_second"] for r in runs]
    p50s = [r["latency_us"]["p50"] for r in runs]
    p95s = [r["latency_us"]["p95"] for r in runs]
    p99s = [r["latency_us"]["p99"] for r in runs]
    cpu_percents = [r.get("server_cpu_percent_one_core", 0.0) for r in runs]

    return {
        "run_count": len(runs),
        "throughput_ops_per_second": {
            "min": round(min(throughputs), 2),
            "max": round(max(throughputs), 2),
            "mean": round(statistics.mean(throughputs), 2),
            "median": round(statistics.median(throughputs), 2),
            "stdev": round(statistics.stdev(throughputs), 2) if len(throughputs) > 1 else 0.0,
            "unit": "completed_pongs_per_second",
        },
        "latency_p50_us": {
            "median": round(statistics.median(p50s), 2),
            "min": min(p50s),
            "max": max(p50s),
        },
        "latency_p95_us": {
            "median": round(statistics.median(p95s), 2),
            "min": min(p95s),
            "max": max(p95s),
        },
        "latency_p99_us": {
            "median": round(statistics.median(p99s), 2),
            "min": min(p99s),
            "max": max(p99s),
        },
        "server_cpu_percent_one_core": {
            "mean": round(statistics.mean(cpu_percents), 2),
            "median": round(statistics.median(cpu_percents), 2),
        },
    }


def prepare_afxdp_environment(temp_path):
    """Prepare runtime directory and config files for mTCP AF_XDP."""
    runtime_dir = temp_path / "runtime" / "mtcp" / "src"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    config_dir = runtime_dir / "config"
    config_dir.mkdir(parents=True, exist_ok=True)

    route_file = config_dir / "route.conf"
    route_file.write_text("ROUTES 1\n198.18.0.0/30 vmtcp1\n")

    arp_file = config_dir / "arp.conf"
    arp_file.write_text("ARP_ENTRY 1\n198.18.0.2/32 d2:ce:d7:4a:a8:37\n")

    afxdp_symlink = temp_path / "runtime" / "afxdp"
    target_afxdp = ROOT / ".deps" / "mtcp-af_xdp" / "source" / "afxdp"
    if not afxdp_symlink.exists():
        afxdp_symlink.symlink_to(target_afxdp)

    conf_file = temp_path / "afxdp_test.conf"
    conf_file.write_text("io = afxdp\nnum_cores = 1\nport = vmtcp1\n")

    return runtime_dir, conf_file


def start_server(backend, host, port, temp_path):
    """Start the specified server backend inside velora-af-server."""
    server_log = temp_path / f"{backend}_server.log"
    log_fp = server_log.open("w")

    if backend == "native":
        cmd = [
            "sudo", "-n", "ip", "netns", "exec", "velora-af-server",
            "env", "VR_LOG_LEVEL=OFF", "VR_LOG_PATH=/dev/null",
            str(NATIVE_BIN)
        ]
        proc = subprocess.Popen(cmd, cwd=temp_path, stdout=log_fp, stderr=subprocess.STDOUT, text=True)
        # Probe for readiness
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"Native server exited prematurely (status {proc.returncode})")
            try:
                probe_cmd = ["sudo", "-n", "ip", "netns", "exec", "velora-af-test1",
                             "python3", "-c",
                             f"import socket; s = socket.create_connection(('{host}', {port}), timeout=0.2); s.close()"]
                res = subprocess.run(probe_cmd, capture_output=True, text=True)
                if res.returncode == 0:
                    break
            except Exception:
                pass
            time.sleep(0.1)
        else:
            raise RuntimeError(f"Native server failed to listen on {host}:{port} within 5s")

        # Find actual velora pid
        out = subprocess.check_output(["pgrep", "-x", "velora"], text=True).strip().splitlines()
        server_pid = int(out[-1])
        return proc, server_pid, log_fp

    elif backend == "afxdp":
        runtime_dir, conf_file = prepare_afxdp_environment(temp_path)
        cmd = [
            "sudo", "-n", "ip", "netns", "exec", "velora-af-server",
            "env", f"MTCP_RUNTIME_DIR={runtime_dir}", "VR_LOG_LEVEL=OFF", "VR_LOG_PATH=/dev/null",
            str(LAUNCHER), str(conf_file), host, str(port)
        ]
        proc = subprocess.Popen(cmd, cwd=temp_path, stdout=log_fp, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"mTCP server exited prematurely (status {proc.returncode})")
            try:
                probe_cmd = ["sudo", "-n", "ip", "netns", "exec", "velora-af-test1",
                             "python3", "-c",
                             f"import socket; s = socket.create_connection(('{host}', {port}), timeout=0.2); s.close()"]
                res = subprocess.run(probe_cmd, capture_output=True, text=True)
                if res.returncode == 0:
                    break
            except Exception:
                pass
            time.sleep(0.1)
        else:
            raise RuntimeError(f"mTCP server did not become ready within 15s")

        out = subprocess.check_output(["pgrep", "-x", "velora-mtcp"], text=True).strip().splitlines()
        server_pid = int(out[-1])
        return proc, server_pid, log_fp

    else:
        raise ValueError(f"Unknown backend: {backend}")


def stop_server(backend, proc, server_pid, log_fp):
    """Gracefully terminate the server and ensure XDP is detached."""
    try:
        subprocess.run(["sudo", "-n", "kill", "-TERM", str(server_pid)], check=False)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        else:
            subprocess.run(["sudo", "-n", "kill", "-9", str(server_pid)], check=False)
            proc.wait(timeout=2.0)
    finally:
        log_fp.close()
        if backend == "afxdp":
            subprocess.run(
                ["sudo", "-n", "ip", "-n", "velora-af-server", "link", "set", "dev", "vmtcp1", "xdp", "off"],
                check=False, capture_output=True
            )


def run_benchmark_session(backend, host, port, connections, pipeline, duration, warmup, repeats, output_path=None):
    """Run full benchmark session against the specified backend."""
    # Build binaries if missing
    if not PINGPONG_BIN.exists():
        subprocess.run(["make", "build/throughput_pingpong"], cwd=ROOT, check=True)
    if backend == "native" and not NATIVE_BIN.exists():
        subprocess.run(["make", "release"], cwd=ROOT, check=True)
    if backend == "afxdp" and not MTCP_BIN.exists():
        subprocess.run(["make", "velora-mtcp"], cwd=ROOT, check=True)

    with tempfile.TemporaryDirectory(prefix=f"velora-bench-{backend}-") as temp_dir:
        temp_path = Path(temp_dir)
        # Ensure root can write
        subprocess.run(["sudo", "-n", "chmod", "777", str(temp_path)], check=True)

        print(f"\n[{backend.upper()}] Starting server on {host}:{port}...")
        proc, server_pid, log_fp = start_server(backend, host, port, temp_path)
        print(f"[{backend.upper()}] Server running with PID {server_pid}.")

        runs = []
        try:
            for run_idx in range(1, repeats + 1):
                print(f"[{backend.upper()}] Run {run_idx}/{repeats} (conns={connections}, pipeline={pipeline}, warmup={warmup}s, duration={duration}s)...")
                client_cmd = [
                    "sudo", "-n", "ip", "netns", "exec", "velora-af-test1",
                    str(PINGPONG_BIN), host, str(port), str(connections), str(pipeline), str(duration), str(warmup)
                ]

                before_proc_cpu = process_cpu_seconds(server_pid)
                before_sys_cpu = system_cpu_counters()
                start_time = time.monotonic()

                client_proc = subprocess.run(
                    client_cmd,
                    capture_output=True,
                    text=True,
                    timeout=duration + warmup + 30
                )
                elapsed_wall = time.monotonic() - start_time
                after_proc_cpu = process_cpu_seconds(server_pid)
                after_sys_cpu = system_cpu_counters()

                if client_proc.returncode != 0:
                    raise RuntimeError(
                        f"Client failed with status {client_proc.returncode}:\n{client_proc.stdout}\n{client_proc.stderr}"
                    )

                run_data = parse_client_output(client_proc.stdout, connections)
                run_data["run_index"] = run_idx
                run_data["wall_seconds"] = round(elapsed_wall, 2)
                cpu_seconds = max(0.0, after_proc_cpu - before_proc_cpu)
                run_data["server_cpu_seconds"] = round(cpu_seconds, 3)
                run_data["server_cpu_percent_one_core"] = round(
                    (cpu_seconds / run_data["measured_seconds"]) * 100.0 if run_data["measured_seconds"] > 0 else 0.0,
                    2
                )
                run_data["host_system_cpu_percent"] = calculate_cpu_percent(before_sys_cpu, after_sys_cpu)

                runs.append(run_data)
                print(f"[{backend.upper()}] Run {run_idx} complete: {run_data['throughput_ops_per_second']:.0f} PONG/s, "
                      f"RTT p50/p95/p99 = {run_data['latency_us']['p50']}/{run_data['latency_us']['p95']}/{run_data['latency_us']['p99']} us, "
                      f"Server CPU = {run_data['server_cpu_percent_one_core']}%")

        finally:
            print(f"[{backend.upper()}] Stopping server...")
            stop_server(backend, proc, server_pid, log_fp)
            print(f"[{backend.upper()}] Server stopped.")

        summary = compute_aggregate_stats(runs)

        full_results = {
            "metadata": {
                "backend": backend,
                "commit": get_git_commit(),
                "kernel": platform.release(),
                "cpu_model": get_cpu_model(),
                "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "host": host,
                "port": port,
                "connections": connections,
                "pipeline": pipeline,
                "warmup_seconds": warmup,
                "duration_seconds": duration,
                "repeats": repeats,
            },
            "runs": runs,
            "aggregate_summary": summary,
        }

        if output_path:
            out_file = Path(output_path)
            out_file.parent.mkdir(parents=True, exist_ok=True)
            out_file.write_text(json.dumps(full_results, indent=2))
            print(f"[{backend.upper()}] Results saved to {output_path}")

        return full_results


def run_parser_test():
    """Unit test for the output parser using synthetic benchmark output."""
    synthetic_output = """
========================================================================
 VELORA BLACK-BOX PING/PONG THROUGHPUT TEST
========================================================================

[CONFIG] host              : 198.18.0.1
[CONFIG] port              : 22409
[CONFIG] connections       : 50
[CONFIG] pipeline/conn     : 8 PINGs
[CONFIG] packet size       : 8 bytes
[CONFIG] duration          : 5 seconds
[CONFIG] warmup            : 2 seconds

[SETUP COMPLETE]
  Requested connections : 50
  Established           : 50

[WARMUP COMPLETE] Starting measured window of 5 seconds...

========================================================================
 FINAL RESULTS
========================================================================

  Benchmark runtime       : 5.01 s
  Warmup duration         : 2 s
  Requested connections   : 50
  Connections alive       : 50
  Connection failures     : 0
  Registration failures   : 0
  Pipeline depth          : 8 PINGs/connection

  Completed PING/PONGs    : 450000
  Average throughput      : 89820 completed msg/s
  RTT p50/p95/p99         : 340 / 512 / 820 us
  RTT samples              : 450000 (1 us buckets; >=100000 us overflow)
  Total errors            : 0
  Requests in flight at end: 120 (not counted as completed)
  Average completions/conn: 9000

[RESULT] PASS - all connections remained healthy

[INTERPRETATION]
  Throughput = valid PONGs received / measured runtime.
  This is an application-level black-box measurement.
========================================================================
"""
    result = parse_client_output(synthetic_output, 50)
    assert result["requested_connections"] == 50, "requested_connections mismatch"
    assert result["active_connections"] == 50, "active_connections mismatch"
    assert result["completed_transactions"] == 450000, "completed_transactions mismatch"
    assert result["throughput_ops_per_second"] == 89820.0, "throughput mismatch"
    assert result["latency_us"]["p50"] == 340, "p50 mismatch"
    assert result["latency_us"]["p95"] == 512, "p95 mismatch"
    assert result["latency_us"]["p99"] == 820, "p99 mismatch"
    assert result["latency_us"]["scope"] == "per_ping_rtt", "latency scope mismatch"
    assert result["errors"] == 0, "errors mismatch"
    assert result["warmup_seconds"] == 2, "warmup_seconds mismatch"
    assert result["in_flight_at_end"] == 120, "in_flight mismatch"

    # Test failure on error
    failing_output = synthetic_output.replace("Total errors            : 0", "Total errors            : 5")
    try:
        parse_client_output(failing_output, 50)
        assert False, "Parser should have raised RuntimeError on errors > 0"
    except RuntimeError:
        pass

    dummy_runs = [
        {"throughput_ops_per_second": 100000.0, "latency_us": {"p50": 300, "p95": 500, "p99": 800}, "server_cpu_percent_one_core": 75.0},
        {"throughput_ops_per_second": 104000.0, "latency_us": {"p50": 310, "p95": 520, "p99": 810}, "server_cpu_percent_one_core": 78.0},
        {"throughput_ops_per_second": 98000.0, "latency_us": {"p50": 290, "p95": 490, "p99": 790}, "server_cpu_percent_one_core": 74.0},
    ]
    summary = compute_aggregate_stats(dummy_runs)
    assert summary["throughput_ops_per_second"]["median"] == 100000.0, "median throughput mismatch"
    assert summary["latency_p50_us"]["median"] == 300, "median p50 mismatch"
    print("ALL PARSER AND STATISTICS UNIT TESTS PASSED (SYNTHETIC VERIFICATION).")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-parser", action="store_true", help="Run local unit tests of output parser and exit")
    parser.add_argument("--backend", choices=["native", "afxdp"], help="Server backend to test")
    parser.add_argument("--host", default="198.18.0.1", help="Target server IPv4 address")
    parser.add_argument("--port", type=int, default=22409, help="Target server TCP port")
    parser.add_argument("--connections", type=int, default=50, help="Concurrent client connections")
    parser.add_argument("--pipeline", type=int, default=8, help="Pipeline depth per connection")
    parser.add_argument("--warmup", type=int, default=2, help="Warmup duration in seconds")
    parser.add_argument("--duration", type=int, default=5, help="Measurement window duration in seconds")
    parser.add_argument("--repeats", type=int, default=1, help="Number of benchmark iterations")
    parser.add_argument("--output", help="Path to write JSON benchmark results")
    args = parser.parse_args()

    if args.test_parser:
        return run_parser_test()

    if not args.backend:
        parser.error("--backend is required when not running --test-parser")

    results = run_benchmark_session(
        backend=args.backend,
        host=args.host,
        port=args.port,
        connections=args.connections,
        pipeline=args.pipeline,
        duration=args.duration,
        warmup=args.warmup,
        repeats=args.repeats,
        output_path=args.output
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

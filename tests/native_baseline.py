#!/usr/bin/env python3
"""Run a repeatable native loopback PING/PONG baseline and emit JSON."""

import argparse
import json
import os
import platform
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENT_SOURCE = ROOT / "tests" / "throughput_pingpong.c"
SERVER_BINARY = ROOT / "build" / "velora"
CLIENT_FLAGS = ["-D_POSIX_C_SOURCE=200809L", "-O3", "-Wall", "-Wextra", "-Wpedantic"]

PATTERNS = {
    "requested_connections": r"Requested connections\s*:\s*(\d+)",
    "established_connections": r"Established\s*:\s*(\d+)",
    "active_connections": r"Connections alive\s*:\s*(\d+)",
    "connection_failures": r"Connection failures\s*:\s*(\d+)",
    "registration_failures": r"Registration failures\s*:\s*(\d+)",
    "completed_transactions": r"Completed PING/PONGs\s*:\s*(\d+)",
    "throughput_ops_per_second": r"Average throughput\s*:\s*([0-9.]+)",
    "latency_us": r"RTT p50/p95/p99\s*:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)",
    "latency_samples": r"RTT samples\s*:\s*(\d+)",
    "errors": r"Total errors\s*:\s*(\d+)",
    "in_flight_at_end": r"Requests in flight at end:\s*(\d+)",
    "measured_seconds": r"Benchmark runtime\s*:\s*([0-9.]+)",
}


def command_output(command):
    try:
        completed = subprocess.run(
            command, check=False, capture_output=True, text=True, timeout=10
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return completed.stdout.strip() if completed.returncode == 0 else None


def cpu_model():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def process_cpu_seconds(pid):
    stat = Path(f"/proc/{pid}/stat").read_text()
    fields = stat[stat.rfind(")") + 2 :].split()
    ticks = int(fields[11]) + int(fields[12])
    return ticks / os.sysconf("SC_CLK_TCK")


def system_cpu_counters():
    counters = []
    try:
        for line in Path("/proc/stat").read_text().splitlines():
            fields = line.split()
            if not fields or not re.fullmatch(r"cpu\d+", fields[0]):
                continue
            values = [int(value) for value in fields[1:]]
            idle = values[3] + (values[4] if len(values) > 4 else 0)
            counters.append((fields[0], sum(values), idle))
    except (OSError, ValueError):
        return None
    return counters


def system_cpu_percent_by_core(before, after):
    if before is None or after is None:
        return None
    before_by_cpu = {name: (total, idle) for name, total, idle in before}
    result = {}
    for name, total_after, idle_after in after:
        if name not in before_by_cpu:
            continue
        total_before, idle_before = before_by_cpu[name]
        total_delta = total_after - total_before
        idle_delta = idle_after - idle_before
        result[name] = (
            100.0 * (total_delta - idle_delta) / total_delta
            if total_delta > 0
            else 0.0
        )
    return result


def parse_result(output, expected_clients):
    result = {}
    for key, pattern in PATTERNS.items():
        match = re.search(pattern, output)
        if match is None:
            raise RuntimeError(f"benchmark output missing {key}")
        values = match.groups()
        if key == "latency_us":
            result[key] = {
                "p50": int(values[0]),
                "p95": int(values[1]),
                "p99": int(values[2]),
                "unit": "microseconds",
            }
        elif key == "throughput_ops_per_second":
            result[key] = float(values[0])
        elif key == "measured_seconds":
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
            "benchmark workload failed correctness checks:\n" + output[-5000:]
        )
    return result


def run_client(client_binary, args, server_process, measure_cpu):
    command = [
        str(client_binary), args.host, str(args.port), str(args.clients),
        str(args.pipeline), str(measure_cpu),
    ]
    before_cpu = process_cpu_seconds(server_process.pid)
    before_system_cpu = system_cpu_counters()
    started = time.monotonic()
    completed = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=measure_cpu + 45,
    )
    elapsed = time.monotonic() - started
    after_cpu = process_cpu_seconds(server_process.pid) if server_process.poll() is None else before_cpu
    after_system_cpu = system_cpu_counters()

    if server_process.poll() is not None:
        raise RuntimeError(f"Velora exited during benchmark (status {server_process.returncode})")
    if completed.returncode != 0:
        raise RuntimeError(
            f"benchmark client exited {completed.returncode}:\n"
            f"{completed.stdout[-3000:]}\n{completed.stderr[-3000:]}"
        )

    result = parse_result(completed.stdout, args.clients)
    result["wall_seconds_including_client_setup"] = elapsed
    cpu_seconds = max(0.0, after_cpu - before_cpu)
    result["server_cpu_seconds_including_setup"] = cpu_seconds
    result["server_cpu_percent_one_core"] = (
        cpu_seconds / elapsed * 100.0
        if elapsed > 0
        else 0.0
    )
    result["server_cpu_percent_machine_capacity"] = (
        result["server_cpu_percent_one_core"] / (os.cpu_count() or 1)
    )
    result["host_system_cpu_percent_by_core"] = system_cpu_percent_by_core(
        before_system_cpu, after_system_cpu
    )
    return result


def wait_for_server(server_process, host, port, server_log, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if server_process.poll() is not None:
            raise RuntimeError(
                f"Velora failed to start (status {server_process.returncode})"
            )
        try:
            with socket.create_connection((host, port), timeout=0.2):
                if server_process.poll() is not None:
                    raise RuntimeError("Velora exited after accepting readiness connection")
                return
        except OSError:
            time.sleep(0.05)
    log = server_log.read_text(errors="replace")[-3000:]
    raise RuntimeError(f"Velora did not listen on {host}:{port}:\n{log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=22409)
    parser.add_argument("--clients", type=int, default=100)
    parser.add_argument("--pipeline", type=int, default=8)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--duration", type=int, default=5)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()

    if not (1 <= args.port <= 65535 and args.clients > 0 and args.pipeline > 0
            and args.warmup > 0 and args.duration > 0 and args.repeats >= 2):
        parser.error("port/clients/pipeline/warmup/duration must be positive; repeats must be >= 2")

    build = subprocess.run(
        ["make", "release"], cwd=ROOT, capture_output=True, text=True
    )
    if build.returncode != 0:
        raise RuntimeError(f"make release failed:\n{build.stdout}\n{build.stderr}")

    compiler = shutil.which("gcc") or "gcc"
    with tempfile.TemporaryDirectory(prefix="velora-native-bench-") as temp_dir:
        temp_path = Path(temp_dir)
        client_binary = temp_path / "throughput_pingpong"
        compile_client = subprocess.run(
            [compiler, *CLIENT_FLAGS, "-o", str(client_binary), str(CLIENT_SOURCE)],
            cwd=ROOT, capture_output=True, text=True,
        )
        if compile_client.returncode != 0:
            raise RuntimeError(
                f"benchmark client compilation failed:\n"
                f"{compile_client.stdout}\n{compile_client.stderr}"
            )

        server_log = temp_path / "server.log"
        server_output = server_log.open("w")
        server_process = subprocess.Popen(
            [str(SERVER_BINARY)], cwd=temp_path, stdout=server_output,
            stderr=subprocess.STDOUT, text=True,
        )
        try:
            wait_for_server(server_process, args.host, args.port, server_log)
            warmup = run_client(
                client_binary, args, server_process, args.warmup
            )
            runs = [
                run_client(client_binary, args, server_process, args.duration)
                for _ in range(args.repeats)
            ]
        finally:
            if server_process.poll() is None:
                server_process.send_signal(signal.SIGTERM)
                try:
                    server_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server_process.kill()
                    server_process.wait(timeout=2)
            server_output.close()

    rates = [run["throughput_ops_per_second"] for run in runs]
    median_rate = statistics.median(rates)
    median_abs_deviation = statistics.median(
        abs(rate - median_rate) for rate in rates
    )
    cpu_count = os.cpu_count() or 1
    result = {
        "benchmark": "native_velora_ping_pong",
        "throughput_unit": "valid PONGs received per measured second (completed PING/PONG transactions)",
        "request": {"type": "PING", "payload_bytes": 0, "wire_bytes": 8},
        "response": {"type": "PONG", "payload_bytes": 0, "wire_bytes": 8},
        "workload": {
            "host": args.host,
            "port": args.port,
            "clients": args.clients,
            "outstanding_per_client": args.pipeline,
            "max_outstanding_requests": args.clients * args.pipeline,
            "warmup_seconds": args.warmup,
            "measurement_seconds_per_run": args.duration,
            "repeats": args.repeats,
            "connection_setup_included_in_measurement": False,
        },
        "build": {
            "server_command": "make release",
            "server_run_command": str(SERVER_BINARY),
            "client_command": "gcc " + " ".join(CLIENT_FLAGS) + " -o <temp>/throughput_pingpong tests/throughput_pingpong.c",
            "release_flags": "-O3 -Wall -Wextra -Iinclude -pthread",
            "client_flags": " ".join(CLIENT_FLAGS),
            "compiler_version": command_output([compiler, "--version"]),
            "commit": command_output(["git", "rev-parse", "HEAD"]),
        },
        "environment": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "kernel": platform.release(),
            "cpu_model": cpu_model(),
            "logical_cpus": cpu_count,
            "interfaces": command_output(["ip", "-brief", "link"]),
            "server_cpu_measurement": "server process user+system CPU from /proc over full client invocation, including connection setup; percentages are normalized to one logical CPU and total logical CPU capacity",
            "per_core_cpu_measurement": "host-wide /proc/stat busy percentage per logical CPU over the full client invocation; includes all processes and is not server-attributed",
        },
        "warmup": warmup,
        "runs": runs,
        "summary": {
            "median_completed_transactions_per_second": median_rate,
            "min_completed_transactions_per_second": min(rates),
            "max_completed_transactions_per_second": max(rates),
            "median_absolute_deviation_transactions_per_second": median_abs_deviation,
            "server_cpu_percent_one_core_median": statistics.median(
                run["server_cpu_percent_one_core"] for run in runs
            ),
        },
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"native baseline failed: {error}", file=sys.stderr)
        raise SystemExit(1)

# Velora — Coding Agent Instructions

## Project

Velora is a C-based event-driven protocol engine. The project is evolving toward a multithreaded architecture and may integrate mTCP with an AF_XDP packet-I/O backend.

## Engineering rules

1. Inspect the existing implementation before changing it.
2. Trace callers, callees, ownership, and error handling before modifying a function.
3. Preserve existing protocol compatibility unless a change is explicitly required.
4. Do not duplicate TCP/IP functionality that belongs to mTCP.
5. Do not assume mTCP API semantics, threading guarantees, or AF_XDP capabilities; verify them in source and documentation.
6. Make the smallest coherent change that solves the task.
7. Add regression tests for bug fixes and new behavior.
8. Build and run relevant tests after modifications.
9. Never claim a test passed unless it was actually run.
10. Never claim AF_XDP zero-copy without verifying the effective runtime mode.
11. Do not run commands with elevated privileges, modify network interfaces, attach XDP programs, or change host networking without explicit approval.
12. Never overwrite unrelated user changes or discard uncommitted work.

## Workflow

For each task:

1. Inspect the relevant source and current Git state.
2. State the implementation plan and important assumptions.
3. Implement only the requested task.
4. Build and test.
5. Inspect the final diff for unintended changes.
6. Report files changed, tests run, results, and remaining risks.

Stop and report if a required assumption cannot be verified, the existing design conflicts with the task, or a change would require an unapproved architectural rewrite.

Prefer correctness, explicit ownership, and measurable performance over speculative optimization.
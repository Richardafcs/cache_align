# HPC Concurrency and Systems Primitives

A focused set of production-style C++20 examples for ultra-low-latency systems: lock-free queues, RCU, seqlocks, memory pools, SIMD scans, and kernel-bypass style RX polling. Each file is self-contained and builds into a standalone binary.

## What’s included

- `hpc_kernel_bypass_rx.cpp`: Kernel-bypass RX polling loop with ownership bits and zero-copy ring semantics.
- `hpc_memory_pool.cpp`: ABA-safe, lock-free fixed-capacity memory pool with cache-line alignment.
- `hpc_mpmc_queue.cpp`: Vyukov-style bounded MPMC queue with per-slot sequence numbers.
- `hpc_seqlock.cpp`: Seqlock with writer fencing and reader retries.
- `hpc_simd_order_search.cpp`: AVX2 SIMD order book scan with branchless selection.
- `hpc_spsc_queue.cpp`: Cache-aligned SPSC ring buffer with relaxed atomics.
- `hpc_tsc_telemetry.cpp`: RDTSCP-based telemetry with thread-local buffers.
- `hpc_urcu.cpp`: Userspace RCU / epoch reclamation for read-mostly workloads.

### Apple Silicon variants

Files ending in `_apple.cpp` tune cache-line sizes and spin/yield behavior for Apple Silicon (arm64). Use those on macOS/arm64; use the non-`_apple` versions on x86_64/Linux or other platforms.

## Build

Requirements: C++20 compiler and `make`.

```bash
make
```

Binaries are produced in `bin/` with the same basename as the source file.

## Run

```bash
./bin/hpc_mpmc_queue
./bin/hpc_spsc_queue
./bin/hpc_tsc_telemetry
```

Most binaries print a short demonstration or benchmark to stdout.

## Benchmarks

```bash
make bench
```

This writes one output file per binary into `benchmarks/` with metadata and results.

For a quick wall-clock benchmark run:

```bash
make bench-smoke
```

This writes a single summary file to `benchmarks/bench_smoke.txt`.

## Tests

```bash
make test
```

This runs a smoke test that executes each binary and checks for expected output markers.

## Notes

- These are educational, production-style implementations intended to show performance patterns and hardware-aware techniques.
- Some binaries use platform-specific instructions (e.g., AVX2, RDTSCP). If your CPU lacks a feature, recompile with appropriate flags or use the fallback code paths where provided.

## Layout

- `*.cpp`: Individual primitives or demos.
- `bin/`: Build outputs.
- `benchmarks/`: Benchmark artifacts from `make bench`.

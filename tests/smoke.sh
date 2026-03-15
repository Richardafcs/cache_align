#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT_DIR/bin"

if [[ ! -d "$BIN_DIR" ]]; then
  echo "bin/ not found. Build first with: make"
  exit 1
fi

run_and_expect() {
  local bin="$1"
  local pattern="$2"

  if [[ ! -x "$bin" ]]; then
    echo "Missing binary: $bin"
    exit 1
  fi

  local output
  if command -v timeout >/dev/null 2>&1; then
    output="$(timeout "${SMOKE_TIMEOUT_SEC:-20}" "$bin")"
  else
    output="$("$bin")"
  fi

  if ! grep -qE "$pattern" <<<"$output"; then
    echo "Output check failed for $bin"
    echo "Expected pattern: $pattern"
    exit 1
  fi
}

run_and_expect "$BIN_DIR/hpc_kernel_bypass_rx" "Zero-copy network layer successfully simulated"
run_and_expect "$BIN_DIR/hpc_memory_pool" "STRESS TEST COMPLETED"
run_and_expect "$BIN_DIR/hpc_mpmc_queue" "MPMC Contention Benchmark Completed"
run_and_expect "$BIN_DIR/hpc_seqlock" "Seqlock Benchmark Completed"
run_and_expect "$BIN_DIR/hpc_simd_order_search" "Performance Results"
run_and_expect "$BIN_DIR/hpc_spsc_queue" "Throughput"
run_and_expect "$BIN_DIR/hpc_tsc_telemetry" "Telemetry Analysis"
run_and_expect "$BIN_DIR/hpc_urcu" "RCU Simulation Completed"

echo "Smoke tests passed."

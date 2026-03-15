#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$ROOT_DIR/bin"
BENCH_DIR="$ROOT_DIR/benchmarks"

mkdir -p "$BENCH_DIR"

OUT_FILE="$BENCH_DIR/bench_smoke.txt"
{
  echo "Benchmark: bench_smoke"
  echo "Date: $(date -u '+%Y-%m-%d %H:%M:%SZ')"
  echo "Platform: $(uname -s) $(uname -m)"
  echo "----------------------------------------"
} > "$OUT_FILE"

run_timed() {
  local name="$1"
  local bin="$2"

  if [[ ! -x "$bin" ]]; then
    echo "Missing binary: $bin" | tee -a "$OUT_FILE"
    return 1
  fi

  local start end elapsed_ms
  start="$(date +%s%3N)"
  "$bin" >/dev/null
  end="$(date +%s%3N)"
  elapsed_ms=$((end - start))

  echo "$name: ${elapsed_ms} ms" | tee -a "$OUT_FILE"
}

run_timed "hpc_kernel_bypass_rx" "$BIN_DIR/hpc_kernel_bypass_rx"
run_timed "hpc_memory_pool" "$BIN_DIR/hpc_memory_pool"
run_timed "hpc_mpmc_queue" "$BIN_DIR/hpc_mpmc_queue"
run_timed "hpc_seqlock" "$BIN_DIR/hpc_seqlock"
run_timed "hpc_simd_order_search" "$BIN_DIR/hpc_simd_order_search"
run_timed "hpc_spsc_queue" "$BIN_DIR/hpc_spsc_queue"
run_timed "hpc_tsc_telemetry" "$BIN_DIR/hpc_tsc_telemetry"
run_timed "hpc_urcu" "$BIN_DIR/hpc_urcu"

echo "Benchmarks written to $OUT_FILE"

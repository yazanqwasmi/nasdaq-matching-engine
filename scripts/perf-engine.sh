#!/usr/bin/env bash
# Microarchitecture profile of the matching-engine hot loop, via Linux perf.
#
# Wraps `bench perf` — a pure FastBook add/cancel/replace/match loop with no
# I/O or timing in the measured region — in `perf stat`, so you can read IPC,
# cache behaviour, and branch prediction for the hot path.
#
# Requires Linux with perf and access to the hardware PMU:
#   sudo sysctl kernel.perf_event_paranoid=1    # (or lower) to allow counters
# Cloud VMs (including GitHub Actions runners) usually do NOT expose PMU
# counters — the hardware events show "<not supported>". Run this on bare metal.
#
# Usage: scripts/perf-engine.sh [path-to-bench]   (default ./build-rel/bench)
set -euo pipefail

BENCH="${1:-./build-rel/bench}"
if [ ! -x "$BENCH" ]; then
  echo "error: bench binary not found or not executable: $BENCH" >&2
  echo "build it first:  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j" >&2
  exit 1
fi
if ! command -v perf >/dev/null 2>&1; then
  echo "error: 'perf' not found (install linux-tools for your kernel)." >&2
  exit 1
fi

# IPC (instructions/cycles), last-level + L1 D-cache misses, branch prediction.
events="task-clock,instructions,cycles,cache-references,cache-misses,\
L1-dcache-loads,L1-dcache-load-misses,branches,branch-misses"

# Pin to a single core for a stable measurement (no root needed).
run=("$BENCH" perf)
if command -v taskset >/dev/null 2>&1; then
  run=(taskset -c 2 "${run[@]}")
fi

echo "+ perf stat --repeat 5 -e $events -- ${run[*]}"
echo
exec perf stat --repeat 5 -e "$events" -- "${run[@]}"

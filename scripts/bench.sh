#!/usr/bin/env bash
# Reproduce the committed benchmark numbers.
#
# The numbers in README.md are only meaningful alongside the machine
# they came from, so this script prints that environment as its first
# output, not as an afterthought. A latency table without its host
# specification is an anecdote.
#
# Run on the benchmark host, not on a laptop:
#   ./scripts/bench.sh [message_count]
#
# Intended for a rented bare-metal instance (AWS c7i.metal or
# c7gn.metal) with the core isolated and the frequency governor
# pinned. See results/ENVIRONMENT.md.

set -euo pipefail

MESSAGES="${1:-5000000}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== environment ==="
echo "date        : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "uname       : $(uname -srmo)"
echo "compiler    : $(${CXX:-c++} --version | head -n1)"
echo "build type  : Release"
echo "native arch : ${HFT_NATIVE_ARCH:-off}"

if [[ -r /proc/cpuinfo ]]; then
  echo "cpu         : $(awk -F: '/model name/ {print $2; exit}' /proc/cpuinfo | sed 's/^ //')"
  echo "cores       : $(nproc)"
  # A busy sibling core adds jitter that has nothing to do with the
  # engine. Report SMT occupancy so a suspicious p999 can be explained.
  if command -v lscpu >/dev/null 2>&1; then
    echo "siblings/core: $(lscpu | awk -F: '/Thread\(s\) per core/ {gsub(/ /,"",$2); print $2}')"
  fi
fi

if [[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
  echo "governor    : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
else
  echo "governor    : (unavailable)"
fi

echo "hugepages    : $(grep -i huge /proc/meminfo | tr -s ' ' | cut -d' ' -f1-5 | tr '\n' ' ')"
echo "messages     : ${MESSAGES}"
echo

echo "=== build ==="
cmake -S "${ROOT}" -B "${ROOT}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${ROOT}/build" --parallel
echo

echo "=== correctness ==="
# Numbers first, then a note that they mean nothing unverified. The
# order matters: a reader should not be able to skip past the caveat.
"${ROOT}/build/hft_differential" || { echo "differential test FAILED"; exit 1; }
# The threaded pipeline must build the same book as the single-threaded
# one. A pipeline that drops or reorders a message looks exactly like a
# speedup in every other column, so this is checked, not assumed.
ctest --test-dir "${ROOT}/build" --output-on-failure \
      -R 'concurrent|pipeline_threaded_equivalence|ring_transfer_integrity|determinism' \
  || { echo "concurrency tests FAILED"; exit 1; }
echo

echo "=== latency and throughput: add-only ingest ==="
"${ROOT}/build/hft_bench" "${MESSAGES}"
echo

echo "=== per-stage latency: shallow vs deep book ==="
"${ROOT}/build/hft_stage_bench" "${MESSAGES}"
echo

echo "=== concurrency: ring vs mutex baseline ==="
# These two place their own threads and report the placement they
# actually achieved, so no taskset is needed here.
"${ROOT}/build/hft_ring_bench" "${MESSAGES}"
echo

echo "=== concurrency: one thread versus two ==="
"${ROOT}/build/hft_pipeline_bench" "${MESSAGES}"

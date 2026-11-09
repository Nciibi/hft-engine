#!/usr/bin/env bash
# Linux performance profiling for the publishable run.
#
# What this is: the perf/tooling half of the benchmark-host
# workflow (scripts/bench.sh measures; this script explains). It
# reports, for the engine's own binaries:
#   * perf stat counters: cycles, instructions, cache misses,
#     TLB misses, branch misses, context switches
#   * perf c2c (AMD): core-to-core transfer / DRAM latency view
#   * NUMA topology, IRQ affinity, isolcpus/nohz_full, hugepages,
#     frequency governor, steal time
#
# Run on the benchmark host (c7i.metal or similar), never on the
# Windows dev box:
#   ./scripts/perf_profile.sh [build_dir] [messages]
#
# Prerequisites: linux-perf (perf), plus one of numactl / lscpu.
#   sudo apt install linux-perf numactl
# perf_event_paranoid must allow user-space profiling, or run
# under sudo for the c2c counters:
#   echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
#
# Nothing here changes system state except reading /proc and
# /sys; isolcpus/governor/affinity are REPORTED, not set. Setting
# them is the host runbook in results/LATENCY.md.

set -uo pipefail

BUILD="${1:-build}"
MESSAGES="${2:-2000000}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/${BUILD}"

echo "=== system ==="
uname -a
echo "cmdline   : $(cat /proc/cmdline 2>/dev/null)"
echo "governor  : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unavailable)"
echo "isolcpus  : $(grep -o 'isolcpus=[^ ]*' /proc/cmdline 2>/dev/null || echo '(not on cmdline)')"
echo "nohz_full : $(grep -o 'nohz_full=[^ ]*' /proc/cmdline 2>/dev/null || echo '(not on cmdline)')"
echo "hugepages : $(grep -i huge /proc/meminfo | tr -s ' ' | cut -d' ' -f1-5 | tr '\n' ' ')"
echo

echo "=== NUMA topology ==="
if command -v numactl >/dev/null 2>&1; then
  numactl --hardware 2>&1 | head -n 12
elif command -v lscpu >/dev/null 2>&1; then
  lscpu | grep -Ei 'socket|core|thread|numa|node|MHz|model' | head -n 12
else
  echo "(no numactl or lscpu: cannot report topology)"
fi
echo

echo "=== IRQ affinity (first 5 IRQs) ==="
for irq in /sys/devices/system/cpu/irq/*/smp_affinity_list; do
  [ -r "$irq" ] || continue
  echo "$(basename "$(dirname "$irq")"): $(cat "$irq")"
done | head -n 5
echo "interrupts: $(head -n 3 /proc/interrupts 2>/dev/null | cut -c1-80)"
echo

echo "=== steal time (cloud hosts: this is the variance) ==="
awk '/^cpu / {print "steal % of first cpu:", $8}' /proc/stat 2>/dev/null
awk '/^cpu / {tot=0; for(i=1;i<=8;i++) tot+=$i; printf "steal fraction (all cpus): %.4f\n", $8/tot}' /proc/stat 2>/dev/null
echo

if ! command -v perf >/dev/null 2>&1; then
  echo "=== perf: NOT INSTALLED ==="
  echo "Install linux-perf and re-run; without perf counters the run"
  echo "is throughput-only, which is exactly the unverified half."
  exit 0
fi

echo "=== perf stat: hft_stage_bench (cache/TLB attribution) ==="
# The counters that decide whether the book update is DRAM-bound
# (order-index probes into 26 MiB tables) or compute-bound.
perf stat -e cycles,instructions,cache-references,cache-misses,LLC-load-misses,dTLB-load-misses,iTLB-load-misses,branch-misses,context-switches,cpu-migrations \
  "${BUILD}/hft_stage_bench" "${MESSAGES}" 2>&1 | tail -n 22
echo

echo "=== perf stat: hft_bench throughput ==="
perf stat -e cycles,instructions,cache-misses,dTLB-load-misses \
  "${BUILD}/hft_bench" "${MESSAGES}" throughput 2>&1 | tail -n 14
echo

echo "=== perf c2c (AMD): cross-core / DRAM latency ==="
# c2c answers the SPSC-ring question directly: are the two threads
# exchanging lines through L3 or through DRAM? On Intel the
# equivalent is `perf c2c -p` too (c2c works on both; the DRAM
# latency view is AMD-only).
if perf stat -e amd_l3_rd_hit_latency,amd_l3_rd_miss_latency "${BUILD}/hft_ring_bench" 1000 \
     >/dev/null 2>&1; then
  perf stat -e amd_l3_rd_hit_latency,amd_l3_rd_miss_latency \
    "${BUILD}/hft_ring_bench" "${MESSAGES}" 2>&1 | tail -n 8
else
  echo "(no AMD L3 latency counters on this host; try:)"
  perf c2c -p "${BUILD}/hft_ring_bench" "${MESSAGES}" 2>&1 | head -n 20
fi
echo

echo "=== perf top 10 (hot lines) ==="
perf record -F 997 -g -- "${BUILD}/hft_stage_bench" "${MESSAGES}" >/dev/null 2>&1 \
  && perf report --no-children --stdio 2>/dev/null | head -n 14 \
  || echo "(perf record failed: check perf_event_paranoid / permissions)"

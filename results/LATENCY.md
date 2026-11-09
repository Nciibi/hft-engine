# Published latency — benchmark host

> **Status: template, not yet run.** The latency cells in `README.md`
> are `[MEASURED]`/withheld until this table is filled from a real
> benchmark-host run. Throughput, ratios and counts are already
> published and do not depend on this file.

This is the runbook for the one measurement this repository cannot
make on the Windows dev host (100 ns clock floor — see
`results/ENVIRONMENT.md`). Everything below is designed to be
completed in one rented bare-metal session: fill the host table,
paste `scripts/bench.sh` output into the stage tables, done.

## Host runbook

Target: AWS `c7i.metal` / `c7gn.metal` (or any bare-metal Linux host
with `rdtsc` and isolated cores). Cost to produce: ~$1–5/hour
spot for the benchmark + perf runs below.

```bash
# 1. Configure the host (isolated core, performance governor).
sudo systemctl set-property cpuidle.governor=performance
sudo cpupower frequency-set -g performance
# isolcpus + nohz_full go on the bootloader cmdline (e.g. GRUB):
#   isolcpus=4 nohz_full=<core-4-SMT-sibling>
# Hugepages: echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# IRQ affinity: move NIC IRQs off the benchmark core:
#   echo 1 | sudo tee /sys/devices/system/cpu/irq/<nic-irq>/smp_affinity_list

# 2. Build + verify + benchmark (order is the point: correctness gates first).
./scripts/bench.sh 5000000

# 3. Attribute the numbers (cache/TLB/NUMA/c2c).
./scripts/perf_profile.sh build 2000000

# 4. Steal time: report /proc/stat steal % before and after the run.
#    AWS does not isolate: low-double-digit-microsecond time sync and
#    steal are normal. Absolute sub-10us figures are not defensible on
#    AWS -- say so in the table notes rather than hiding it.
```

Then paste the `=== environment ===` block into the host table and
the stage tables below. Do not hand-edit a number into `README.md`
without the matching environment record.

## Host (fill from bench.sh output)

| Field | Value |
|---|---|
| Instance type | |
| CPU model | |
| Cores / threads | |
| RAM | |
| Kernel | |
| Hugepages | |
| Isolated cores | |
| Frequency governor | |
| SMT control | |
| Compiler + version | |
| Compile flags | |
| Date | |
| Steal time (pre/post) | |

## Per-stage latency (fill from bench.sh output)

p50 / p99 / p999, single thread. Read each cell against the
`hft_tsc_bench` floor printed above it: a figure at or below the
clock-pair p50 is *unresolved*, and the honest entry is
"below the measurement floor". `end to end` is a single pipeline
pass, not the sum of the rows. Encode is N/A (ITCH is outbound-only).

**Shallow book — 10 price levels a side**

| Stage | p50 | p99 | p999 |
|---|---|---|---|
| Decode | | | |
| Book update | | | |
| Decision | | | |
| Risk check | | | |
| End to end | | | |

**Deep book — 1000 price levels a side**

| Stage | p50 | p99 | p999 |
|---|---|---|---|
| Decode | | | |
| Book update | | | |
| Decision | | | |
| Risk check | | | |
| End to end | | | |

## Repeatability (fill from bench.sh REPEAT runs)

| Run | Shallow throughput (msg/s) | Deep throughput (msg/s) |
|---|---|---|
| 1 | | |
| 2 | | |
| 3 | | |
| Spread (max/min − 1) | | |

Expected spread to report honestly: ~16% on bare metal, ~150% in
cloud. A table with one run and no spread is a cherry-pick.

## Perf attribution (fill from perf_profile.sh output)

| Counter (hft_stage_bench) | Value |
|---|---|
| LLC-load-misses / 1k instr | |
| dTLB-load-misses / 1k instr | |
| Branch-misses / 1k instr | |
| Context-switches / 1k instr | |

Expected shape (dev host): LLC misses dominate — four dependent
DRAM accesses per book update (see `results/OPTIMIZATION.md`).
If the benchmark host disagrees, that disagreement is the finding.

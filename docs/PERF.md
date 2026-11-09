# Linux performance tooling — the entry-level gap, closed on paper

The Akuna/Citadel entry-level JD list names `perf`, `perf c2c`,
cache-miss and TLB attribution, NUMA topology, IRQ affinity,
`isolcpus`/`nohz_full`, hugepages, and GDB/Valgrind/AddressSanitizer.
This document maps each skill to the concrete command, what it
should show *for this engine*, and where the result lands
(`results/LATENCY.md`, `results/ENVIRONMENT.md`). `scripts/perf_profile.sh`
runs the automated half; the interpretation is yours to know cold.

> All commands assume the benchmark host (Linux). Nothing here
> runs on the Windows dev box — that is the gap this file closes
> *procedurally*: the commands are written, checked against
> `scripts/bench.sh`'s environment block, and ready to run.

## 1. `perf stat` — cache-miss and TLB attribution

```bash
perf stat -e cycles,instructions,cache-references,cache-misses,LLC-load-misses,dTLB-load-misses,branch-misses,context-switches \
  ./build/hft_stage_bench 2000000
```

What it should show for this engine:

- **LLC-load-misses dominate.** Book update is four dependent DRAM
  accesses per message (`order_index_` probe, `level_index_` probe,
  `levels_`, order tail — 26–38 MiB structures, all beyond L3).
  If LLC misses per kilo-instruction is high and cycles/kilo-instruction
  scales with it, the cost is memory *count*, not layout — which is
  exactly what `results/OPTIMIZATION.md` measured and why every
  layout micro-optimisation landed in the noise.
- **dTLB-load-misses matter on the deep book** (1000 levels/side).
  Hugepages (2 MiB) collapse the page walks: see §4.
- **Context-switches near zero** on the single-threaded bench.
  Non-zero switches on a "single-threaded" run means SMT or OS
  interference — find it before quoting p999.

## 2. `perf c2c` — core-to-core transfer cost

```bash
perf c2c -p ./build/hft_ring_bench 2000000
```

What it should show: the SPSC ring hands off cache lines between
two physical cores. Healthy: transfers through L3 at the measured
break-even cost (the pipeline split is 0.98–1.10× — see
`results/ENVIRONMENT.md`). Pathological: lines bouncing through
DRAM, which means the cores are on different NUMA nodes (§3) or an
SMT sibling is contending. `perf c2c` is the tool that turns
"the split is break-even" from a ratio into a mechanism.

## 3. NUMA topology and IRQ affinity

```bash
numactl --hardware          # node count, memory per node, live CPUs
cat /sys/devices/system/cpu/irq/*/smp_affinity_list | head
```

What it should show: on the benchmark host, the bench core and its
memory should be local; NIC IRQs should be off the benchmark core.
Remote-NUMA memory doubles the DRAM latency *per dependent access*,
and this workload is nothing but dependent accesses — so a NUMA
misplacement shows up as a latency regression with no code change,
which is precisely why it's reported (not set) by the scripts:
a run that doesn't record topology can't be reproduced.

## 4. `isolcpus` / `nohz_full` / hugepages / governor

```bash
grep isolcpus /proc/cmdline; grep nohz_full /proc/cmdline
grep -i huge /proc/meminfo
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
# pin: taskset -c 4 ./build/hft_stage_bench 2000000
```

What each does, in one line:

- `isolcpus`: removes the core from the scheduler's load balancer —
  no migrations, no cross-core noise into p999.
- `nohz_full`: no periodic timer tick on the core — the tick is a
  ~1 µs interrupt that lands in the tail.
- Hugepages: 2 MiB pages cut TLB walks (§1) and jitter.
- `performance` governor: no P-state transitions mid-run; a governor
  downshift reads as a slow stage and isn't one.

## 5. GDB / AddressSanitizer — correctness under the perf build

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-fsanitize=address
cmake --build build-asan --parallel
ctest --test-dir build-asan
```

The differential suite under ASan is the most valuable perf-adjacent
run available: it checks the *fast* book against the naive model
with sanitiser-instrumented memory, so a use-after-free that
"only happens under load" is caught deterministically. (GCC ASan
is broken with the PGO toolchain here — see
`results/OPTIMIZATION.md` LTO/PGO note — which is why this uses
a separate build directory and `-fsanitize` directly.)

## Where each result lands

| Tool | Output | Lands in |
|---|---|---|
| `scripts/bench.sh` env block | host table | `results/LATENCY.md`, `README.md` table |
| `scripts/perf_profile.sh` counters | attribution table | `results/LATENCY.md` |
| `hft_tsc_bench` floor | resolvability gate | `results/ENVIRONMENT.md` |
| ASan differential | correctness under instrumentation | CI (second build dir) |

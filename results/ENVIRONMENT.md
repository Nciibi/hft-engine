# Benchmark environment

A latency number without the machine it came from is an anecdote. This
file is the record that makes the table in `README.md` checkable.

## Status: NOT YET THE BENCHMARK HOST

**No committed numbers yet.** The `[MEASURED]` placeholders in
`README.md` are unfilled. The figures below were produced on a
development machine and are recorded here only as a baseline to
compare against, not as a result to quote.

## Development baseline (not for publication)

| Field | Value |
|---|---|
| CPU | AMD Ryzen 5 1600, 6c/12t @ 3.2 GHz |
| RAM | 16 GB |
| OS | Windows 10/11, x86-64 |
| Compiler | GCC 16.2.0 (w64devkit, portable, no admin) |
| Toolchain path | `D:\w64devkit\bin` — gcc, cmake 4.4.3, make 4.4.1, ccache |
| Flags | `-std=c++20 -O3 -DNDEBUG` via `CMAKE_BUILD_TYPE=Release` |
| Native arch | off (`HFT_NATIVE_ARCH=OFF`) |
| Cores used | 1 for the latency tables, 2 distinct physical cores for the concurrency tables |
| Isolation | none: no SMT control, no hugepages, no frequency pinning |
| Date | 2026-10-02 |

### Compiler history, and why it changed

An earlier revision of this file recorded "LLVM/clang 19 (via `zig c++`
0.13.0)". That line was wrong in two ways and both mattered:

- Zig 0.13.0 bundles **clang 18.1.6**, not 19.
- That Zig installation is now **partially extracted and cannot compile
  C++ at all**: `lib/libc/include/generic-mingw`,
  `lib/libc/include/x86_64-windows-gnu` and `libcxx/include/__config_site`
  are absent and `libc/{glibc,wasi,musl,darwin}` are empty directories.
  A trivial C++ file compiles; anything that includes `<compare>` or
  `<vector>` fails with `'compare' file not found`.

GCC 16.2.0 from w64devkit replaced it and is now the development
compiler. This surfaced **twelve pre-existing warnings** that clang
accepted and GCC rejects — mostly `-Wconversion` and `-Wsign-conversion`
— and all twelve are fixed. The `-Werror` policy is now enforced by two
compilers rather than one, which is the point of having it.

### Concurrency reference run (dev machine, 1,000,000 messages)

Recorded to show the *shape* of the concurrency tables, not to be quoted.
See the caveats below and `hft_pipeline_bench`'s own output, which prints
the run-to-run noise floor that governs how these ratios may be read.

```
decode + apply, 1 thread          778 K msg/s   (mixed capture, 603/255 levels)
lock-free ring, 1024 slots      26.4 M msg/s   (vs 431 K msg/s mutex baseline)

K=1    704 K msg/s   0.91x
K=8    628 K msg/s   0.81x
K=32   621 K msg/s   0.80x
K=128  630 K msg/s   0.81x

baseline drift between two identical runs: -4.4%
```

Two threads lose to one at every batch size, and the drift between
identical runs is several percent. The finding is that the decode/apply
split does not pay at this granularity; see the concurrency section of
`README.md`.

### Latency reference run (superseded, add-only feed)

From the previous toolchain, retained only for continuity:

```
decode       p50    1 ns    p99    2 ns    p999    5 ns
book update  p50    2 ns    p99    6 ns    p999   89 ns
total        p50    2 ns    p99    7 ns    p999   89 ns
throughput   4.81 M msg/s
```

Caveats that apply to every figure above:

- **Consumer silicon.** A 2017-era budget hex-core. These are not
  representative of a bare-metal server with isolated cores.
- **No isolation.** A busy SMT sibling inflates the tail. The p999
  above is more likely to be scheduler noise than the engine.
- **decode p50 equals the clock-read p50** (both 1 ns). The decode cost
  is at the noise floor of the measurement, not genuinely free. A
  finer method (rdtsc with fences and TSC calibration, or a
  many-sample interleaved A/B) would resolve it.
- **Concurrency ratios need a noise floor.** Two identical single-thread
  runs on this machine drifted 4.4%. Any claimed difference smaller than
  that is not a result, and `hft_pipeline_bench` prints the drift for
  exactly this reason.

## The benchmark host (to be filled in)

Intended target: AWS `c7i.metal` or `c7gn.metal`.

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

## How to produce a publishable run

```bash
./scripts/bench.sh 5000000
```

The script prints the environment before the results, runs the
differential test before the benchmark, and fails loudly if the
book's pools saturate mid-run. On a real host:

```bash
sudo systemctl set-property cpuidle.governor=performance
sudo cpupower frequency-set -g performance

# Pin to an isolated core, with its SMT sibling idle.
sudo sh -c 'echo 4 > /sys/devices/system/cpu/cpu4/isolated'
sudo sh -c 'echo 1 > /sys/devices/system/cpu/cpu5/online'
taskset -c 4 ./build/hft_bench 5000000
```

Then paste the script's own `=== environment ===` block into the table
above and fill the `README.md` placeholders. Do not hand-edit a number
into the README without the matching environment record; that is the
one failure mode this file exists to prevent.

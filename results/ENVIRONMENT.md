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
| Compiler | LLVM/clang 19 (via `zig c++` 0.13.0), portable zip, no admin |
| Flags | `-std=c++20 -O2` |
| CMake flags | `CMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`) |
| Cores used | 1, single-threaded |
| Isolation | none: no core pinning, no SMT control, no hugepages, no frequency pinning |
| Date | 2026-10-01 |

Reference run, 1,000,000 messages, ladder depth ~3,200 levels per side:

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
- **Compiler differs from the target.** Development used clang;
  the committed runs use MSVC on Windows and GCC on Linux. Expect
  different numbers, and expect the Linux build to be the one quoted.

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

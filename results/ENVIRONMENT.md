# Benchmark environment

A latency number without the machine it came from is an anecdote. This
file is the record that makes the table in `README.md` checkable.

## Status: NO PUBLISHED BENCHMARK NUMBERS YET

**The `[MEASURED]` placeholders in `README.md` are unfilled, deliberately.**
This file records why, and what has to happen first.

The blocker is not scheduling. It is that this development host's clock
has **100 ns granularity**, which makes the fastest pipeline stage
unmeasurable rather than fast. Filling the latency table from this machine
would produce a table of floor readings dressed up as results — the exact
failure this repository documents everywhere else.

Three things must happen before any figure is published:

1. **Run on the rented bare-metal host** (`c7i.metal`), which has a
   TSC-backed clock at nanosecond granularity.
2. **Run `scripts/bench.sh` there.** It gates on the correctness tests and
   on cross-optimisation determinism first, then prints the environment
   block, then the clock characterisation, then the benchmarks — in that
   order.
3. **Paste the environment block into the host table below** and fill the
   `README.md` placeholders from the script's own output. Do not hand-edit
   a number in without the matching environment record; that is the one
   failure mode this file exists to prevent.

The figures in the sections below are development-machine baselines,
recorded so the change is legible and so the host comparison has
something to be different from. They are **not** for publication.

## Development baseline (not for publication)
| Field | Value |
|---|---|
| CPU | AMD Ryzen 5 1600, 6c/12t @ 3.2 GHz |
| RAM | 16 GB |
| OS | Windows 10/11, x86-64 |
| Compiler | GCC 16.2.0 (w64devkit, portable, no admin) |
| Second compiler | clang 23.1.2, `x86_64-w64-windows-gnu`, w64devkit runtime |
| Toolchain path | `D:\w64devkit\bin` — gcc, cmake 4.4.3, make 4.4.1, ccache |
| Flags | `-std=c++20 -O3 -DNDEBUG` via `CMAKE_BUILD_TYPE=Release` |
| Warnings | `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align -Wdouble-promotion -Wnull-dereference` |
| Native arch | off (`HFT_NATIVE_ARCH=OFF`) |
| Cores used | 1 for the latency tables, 2 distinct physical cores for the concurrency tables |
| Isolation | none: no SMT control, no hugepages, no frequency pinning |
| **Clock** | **QPC at 10 MHz — one tick is 100 ns. See below.** |
| Date | 2026-10-02 |

### Compiler history, and why it changed twice

An earlier revision of this file recorded "LLVM/clang 19 (via `zig c++`
0.13.0)". That line was wrong in two ways and both mattered:

- Zig 0.13.0 bundles **clang 18.1.6**, not 19.
- That Zig installation is now **partially extracted and cannot compile
  C++ at all**: `lib/libc/include/generic-mingw`,
  `lib/libc/include/x86_64-windows-gnu` and `libcxx/include/__config_site`
  are absent and `libc/{glibc,wasi,musl,darwin}` are empty directories.
  A trivial C++ file compiles; anything that includes `<compare>` or
  `<vector>` fails with `'compare' file not found`.

GCC 16.2.0 from w64devkit replaced it as the primary compiler, surfacing
**twelve pre-existing warnings** that clang had accepted — mostly
`-Wconversion` and `-Wsign-conversion` — all now fixed.

clang 23.1.2 was then installed properly and added as a second
compiler, which surfaced **two more real defects**. The `-Werror` policy
is now enforced by two compilers, which is the point of having it.

### Concurrency reference run (dev machine, 1,000,000 messages)

Recorded to show the *shape* of the concurrency tables, not to be quoted.
See the caveats below and `hft_pipeline_bench`'s own output, which prints
the run-to-run noise floor that governs how these ratios may be read.

```
lock-free ring, 1024 slots      26.4 M msg/s   (vs 431 K msg/s mutex baseline)
```

Splitting decode from apply through one ring is **break even** at every
batch size from 1 to 128 — within the measured noise floor, which on this
host is under one percent. An earlier revision of `hft_pipeline_bench`
reported 0.81x–0.91x here and blamed load imbalance. That was a bug:
messages queued at shutdown were never applied, so the run did less work
and timed as slower. The load-imbalance story was a rationalisation that
fit the symptom and was wrong about the cause.

Sharding the same work by symbol across 64 books:

```
1 thread, fused, 64 books       3.40 M msg/s   (baseline)
dispatcher + 2 workers          6.85 M msg/s   2.01x   books identical
dispatcher + 3 workers          6.42 M msg/s   1.89x   books identical
dispatcher + 5 workers          6.05 M msg/s   1.78x   books identical
```

Ratios flatten past two workers because the dispatcher decodes and routes
every message and is therefore a serial floor. On a machine with more
cores that floor moves; on a bare-metal host the numbers should be
re-measured rather than extrapolated.

Every sharded row is checked to have produced books byte-identical to the
single-threaded baseline. That check is not a formality — it is what
caught the dropped-message bug above, and what catches a misrouted
mutation, which looks exactly like a speedup.

Two things to take from this beyond the numbers. A benchmark reporting a
*slowdown* deserves the same suspicion as one reporting a speedup. And a
plausible mechanism is not a verified one.

### The clock, and why the old numbers were wrong by 100x

**`QueryPerformanceFrequency` returns 10 MHz on this host, so one QPC
tick is 100 nanoseconds.** This is the single most important line in this
file.

Every benchmark used to record `Timer::now()` deltas straight into a
latency histogram and print the result as nanoseconds. On this machine
that made every latency figure **100x too small**: a reported `decode p50`
of 2 was 200 ns. Throughput was correct, because it was the one figure
computed through a conversion and therefore the only one with an
independent quantity — the wall-clock time the run took — to check it
against.

Fixed in `include/hft/util/timer.hpp`; see
[Phase 8](../docs/BUGS.md#phase-8-every-latency-figure-in-this-repository-was-wrong-by-100x).
The corrected numbers are in the tables below. **Do not quote any figure
from the superseded section.**

### Resolution floor, as measured by `hft_tsc_bench`

| Clock                  | Pair overhead (p50) | Effective floor |
|------------------------|---------------------|-----------------|
| QPC                    | 1 tick = 100 ns     | 100 ns          |
| `rdtsc` + `lfence`     | 97 ticks = ~30 ns   | ~30 ns          |
| `rdtscp` + `lfence`    | 97 ticks = ~30 ns   | ~30 ns          |

Consequences, and they are not small:

- **Decode cannot be measured on this host.** Its p50 is exactly one QPC
  tick, which is the floor rather than a number. Even the TSC's ~30 ns
  floor is well above the cost of decoding a 36-byte frame.
- **Any stage figure must be compared against the clock-pair p50.** A
  figure at or below it is *unresolved*, and the honest entry is "below
  the measurement floor".
- **The TSC is invariant here** — two physical cores agree to six figures —
  so migration does not corrupt a reading on this CPU. `TsClock::invariant()`
  reports this rather than assuming it; it is a property of the host, not
  of the code.
- **Calibrated TSC rate: 3199.98 MHz**, cross-checked against QPC over a
  shared 20 ms window with 0.0001% disagreement.

### Run-to-run drift, and which figure applies to which number

| Workload            | Drift | Qualifies                    |
|---------------------|-------|------------------------------|
| L1-resident (64B copy) | ~14% | per-stage latency ratios   |
| DRAM-resident (4KiB copy) | ~23% | throughput ratios        |

Both measured over matched ~50 ms windows with a discarded warm-up. A
memory-bound workload contends for the memory controller with everything
else on the box, which is why it is the noisier of the two; using either
figure for the other regime is wrong in both directions.

**Any quoted difference smaller than the relevant figure is noise on this
host.** This is why the concurrency tables are described qualitatively
until the benchmark-host run.

### Superseded: figures taken before the Add Order decoder fix

**Do not use any of these.** Two independent reasons, either of which is
sufficient. They were produced from a feed whose Add Order messages were
misdecoded — the decoder read the share count as the price — so the book
held share counts where prices belonged. And every latency figure in this
repository was 100x understated, for the reason above.

They are retained only so the change is legible. If a number appears below
and not in the sections above it, that is why.

```
decode       p50    1 ns    p99    2 ns    p999    5 ns
book update  p50    2 ns    p99    6 ns    p999   89 ns
total        p50    2 ns    p99    7 ns    p999   89 ns
throughput   4.81 M msg/s
```

The ~89 ns p999 in particular belonged to a 3,200-level ladder keyed on
integers in `[1, 500]`. A real ladder is keyed on prices around 1,000,000
raw units, which changes the level distribution, the cache behaviour of
the index, and the tail. The corrected re-measurement has not been run
on the benchmark host yet, which is why the latency table in `README.md`
is still unfilled.

Throughput in that block is the one figure that survived both errors, and
it is the reason the other figures went unnoticed for as long as they did.

### Caveats that apply to every figure in this file

- **Consumer silicon.** A 2017-era budget hex-core. These are not
  representative of a bare-metal server with isolated cores.
- **A 100 ns clock.** The single biggest handicap on this host for
  latency work. A machine with a TSC-backed QPC would have ~20 ns
  granularity, and the decode stage would at least be borderline
  resolvable rather than certainly not.
- **No isolation.** A busy SMT sibling inflates the tail. The p999
  figures here are more likely to be scheduler noise than the engine.
- **Concurrency ratios need a noise floor**, which is 14–23% on this host
  depending on regime. Any claimed difference smaller than that is not a
  result, and `hft_pipeline_bench` prints its own drift measurement for
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
./scripts/bench.sh 5000000          # environment, build, differential test, results
./build/hft_ring_bench              # ring vs mutex baseline
./build/hft_pipeline_bench          # 1 thread vs 2, batch sweep, checksum-matched
```

The script prints the environment before the results, runs the
differential test before the benchmark, and fails loudly if the book's
pools saturate mid-run. On a real host:

```bash
sudo systemctl set-property cpuidle.governor=performance
sudo cpupower frequency-set -g performance

# Pin to an isolated core, with its SMT sibling idle.
sudo sh -c 'echo 4 > /sys/devices/system/cpu/cpu4/isolated'
sudo sh -c 'echo 1 > /sys/devices/system/cpu/cpu5/online'
taskset -c 4 ./build/hft_bench 5000000
```

The concurrency tools place their own threads and report the placement
they achieved, so they need no `taskset`; do run them under
`isolcpus` and `nohz_full` if the host has them configured.

Then paste the script's own `=== environment ===` block into the table
above and fill the `README.md` placeholders. Do not hand-edit a number
into the README without the matching environment record; that is the
one failure mode this file exists to prevent.

### Building this repository

Requires CMake, a C++20 compiler and `git`. Zero external dependencies.

**GCC 16.2.0 and clang 23.1.2 both build the tree clean under the project's
`-Werror` policy, and all 24 tests pass under each.** Two compilers is not
fastidiousness for its own sake: the twelve warnings GCC surfaced when it
replaced clang, and the two clang surfaced when it was added, were both
real defects. See the toolchain table below.

With the w64devkit toolchain used for development:

```bash
export PATH="/d/w64devkit/bin:$PATH"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

#### Second compiler: clang on Windows with a MinGW runtime

clang's default Windows target is `x86_64-pc-windows-msvc`, and with no
MSVC installation present it cannot find the C++ standard library at all
(`fatal error: 'vector' file not found`). Pointing it at the w64devkit
MinGW runtime works, and needs three non-obvious flags:

```powershell
cmake -B build-clang -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" `
  -DCMAKE_CXX_COMPILER_TARGET=x86_64-w64-windows-gnu `
  "-DCMAKE_EXE_LINKER_FLAGS=-static-libgcc" `
  "-DCMAKE_CXX_STANDARD_LIBRARIES=-lwinpthread"
cmake --build build-clang --parallel
ctest --test-dir build-clang
```

Why each flag is required, since all three failures are silent-ish:

- **`CMAKE_CXX_COMPILER_TARGET=x86_64-w64-windows-gnu`** — without it
  CMake configures for the MSVC ABI and the link step fails looking for
  `ole32.lib`, `msvcrtd.lib` and friends.
- **`-static-libgcc`** — w64devkit ships `libgcc.a` and `libgcc_eh.a` but
  no `libgcc_s`, which is clang's default. The error is
  `unable to find library -lgcc_s`.
- **`-lwinpthread` as a standard library, not a linker flag** — the
  project uses `std::mutex` and `std::condition_variable`, and clang does
  not link winpthreads implicitly the way GCC does. It has to come *after*
  `libstdc++` on the link line, which is what
  `CMAKE_CXX_STANDARD_LIBRARIES` does and `CMAKE_EXE_LINKER_FLAGS` does
  not; in the earlier position it produces `undefined symbol:
  pthread_mutex_lock`.

#### What the second compiler found

Both were real, and neither was reachable from the compiler the project
developed on:

- **`Symbol::terminator_` was reported as an unused private field.** It is
  not dead code: it occupies the byte immediately after the eight-byte wire
  field, which is the only reason `c_str()` returns a NUL-terminated
  string. That byte once held the *length* instead, and keying a
  `std::unordered_map<std::string, ...>` on `c_str()` read past the object
  and concluded that all sixty-four symbols in a multi-symbol feed were
  distinct. The right response was `[[maybe_unused]]` with the reason
  recorded, **not** deletion — and it is worth stating that the compiler
  was correct on both counts simultaneously.
- **`kGeneratedSymbol` was an unused constant in `generator.cpp`** while
  the literal `"SIMTEST "` appeared independently as a default argument in
  `generator.hpp`. Two spellings of one wire value is precisely the
  arrangement that let the Add Order price/size swap survive every test, so
  the constant moved to the header and both now name it.

A project that compiles warning-free under one compiler has not finished
reading its warnings. It has finished reading that compiler's.

# HFT Engine

A from-scratch NASDAQ TotalView-ITCH 5.0 order book and market-making
engine in C++20. Fixed-point, arena-backed, zero allocation on the hot
path, deterministic replay, benchmarked to p999 across the full
decode-to-encode pipeline.

> **Status:** reference implementation built for understanding exchange
> mechanics, not for production deployment. See
> [What this is not](#what-this-is-not).

## Results

Measured on `<instance spec>`, `<compiler + flags>`, `<kernel>`.
Reproduce with `./scripts/bench.sh`. Full environment in
[`results/ENVIRONMENT.md`](results/ENVIRONMENT.md).

### Latency, by pipeline stage

Per message, p50 / p99 / p999, single thread, pinned core:

| Stage                | p50           | p99           | p999          |
|----------------------|---------------|---------------|---------------|
| Decode (binary)      | `[MEASURED]`  | `[MEASURED]`  | `[MEASURED]`  |
| Book update          | `[MEASURED]`  | `[MEASURED]`  | `[MEASURED]`  |
| Market data -> trade | `[MEASURED]`  | `[MEASURED]`  | `[MEASURED]`  |
| Risk check           | `[MEASURED]`  | `[MEASURED]`  | `[MEASURED]`  |

### Throughput

| Condition             | msgs/sec        | Depth |
|-----------------------|-----------------|-------|
| Shallow book (10 lvl) | `[MEASURED]`    | 10    |
| Deep book (1k lvl)    | `[MEASURED]`    | 1000  |

Shallow-book throughput is reported separately because it is
meaningless: real books are deep, and a number from an empty book is a
number about your loop, not your engine.

### Correctness

- `[N]` million differential operations against a naive reference model,
  full state comparison after every operation. Zero mismatches.
- Deterministic replay: FNV-1a book state checksum over `[N]` messages.
  Identical across runs, across optimisation levels, across machines.

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/hft_bench          # latency + throughput table
./build/hft_replay feed.itch
./build/hft_test           # differential + unit tests
```

Zero external dependencies in the library target. CMake, a C++20
compiler, and `git`.

## Architecture

```
  feed.itch
      |  mmap, sequential
      v
  [ ITCH decoder ]  zero-copy, big-endian, 48-bit timestamps
      |  typed messages
      v
  [ Order book ]    price-time priority, fixed-point, slab arena
      |
      +---> [ Market data handler ]  book deltas to consumers
      |
      v
  [ Market maker ]  Avellaneda-Stoikov inventory skew
      |
      v
  [ Pre-trade risk ]  position, notional, fat-finger, rate, kill
      |
      v
  [ Order management ]  state machine, acknowledgements
```

## Design decisions

**Fixed-point integers, never floating point.** Prices are `int64_t` at
1/10000, matching ITCH's native precision. Floating point in a price
ladder is a rounding bug waiting for a specific fill size.

**Slab arena, no allocation on the hot path.** Orders are integer
handles into a preallocated arena, never pointers. Allocation is
batched and amortised outside the message loop. This is the same
size-class and freelist reasoning that drives
[allox](https://github.com/Nciibi/allox), my thread-cached allocator.

**ITCH `X` and `D` are different operations.** `X` is a partial cancel:
subtract cancelled shares from the original add. `D` removes the order
entirely. Conflating them is the most common ITCH book bug, and the
naive reference model exists partly to catch it.

**Sequence gaps are detected, never absorbed.** A SOUP sequence gap
means book state is corrupt. Continuing silently produces confident
wrong numbers, which is worse than stopping.

**Unknown message types skip by length.** A live feed will always
contain types this build does not implement. Crashing is not an option.

**Determinism is a feature.** Same feed file, same checksum, any
machine, any optimisation level. Without that property, no benchmark
is reproducible and no bug is reproducible.

## Failure modes

Things this build handles explicitly, because they are where real
systems fail:

| Condition                   | Behaviour                             |
|-----------------------------|---------------------------------------|
| SOUP sequence gap           | Detected, reported, replay can resync |
| Unknown message type        | Skipped by length                     |
| Execute after cancel        | Detected and rejected                 |
| Replace chain / lost order  | Detected and rejected                 |
| Order ID collision          | Detected and rejected                 |
| Risk limit breach           | Order rejected, never reaches the wire|
| Clock skew                  | Documented as out of scope; see below |

## This maps to interview questions

If you are evaluating this repository, the three questions it was
built to answer are:

1. **Design a low-latency market data handler.** Phases 2 and 6.
2. **Design an OMS that stays correct under high message rates.**
   Phases 1 and 3, with the differential test as the correctness
   argument.
3. **Design a matching engine.** Phases 1, 4 and 5.

## What this is not

Stated plainly, because a reference implementation that pretends to be
production software is worse than one that does not:

- **No live exchange connectivity.** TotalView-ITCH requires a Nasdaq
  market-data agreement. This runs on captured or generated feeds.
- **No multi-shard scaling.** Designed for one instrument per book.
  Sharding by symbol needs a sequencer and a partition scheme that are
  not here.
- **No clock synchronisation.** No PTP, no NTP discipline, no
  cross-machine timestamp alignment. Cross-host latency claims would
  be meaningless without it.
- **No persistence or recovery.** A process restart loses all state.
  A real system needs a write-ahead log and a snapshot cadence.
- **No self-trade prevention, no auction handling, no order book
  state message processing.**
- **Benchmarks are single-socket.** No kernel bypass, no io_uring, no
  DPDK. Real shops measure the syscall layer separately because it
  dominates.

## Reference

NASDAQ TotalView-ITCH 5.0 interface specification, for the message
layouts, the big-endian field encoding, and the partial-cancel rules.
Market making follows Avellaneda-Stoikov; inventory risk handling
follows Guéant-Lehalle-Fernandez-Tapia.

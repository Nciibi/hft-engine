# Optimisation log

Every entry here was measured on the development host with
`scripts/ab.ps1`, and every entry is kept — including the ones that failed.
A log containing only successful optimisations is a log of confirmations,
not of engineering, and the failures are where the information is.

## The instrument was wrong before the engine was

The most consequential thing found in this session was not about the engine
at all, and it invalidated part of the analysis above.

`hft_bench` sizes its order and level pools from the message count and
**never removes an order**, so its book grows without bound. Cost per
message therefore never converges — it keeps falling as the run gets
longer, because an increasing share of the runtime is steady-state work
rather than pool page-faulting:

| messages | working set | ns/message |
|---|---|---|
| 50,000 | 6 MiB | 1614.7 |
| 200,000 | 23 MiB | 763.0 |
| 800,000 | 93 MiB | 333.4 |
| 3,200,000 | 372 MiB | 156.1 |
| 6,400,000 | ~950 MiB | ~91 |

Decode-only, measured over the same sizes, is **flat at 38.5 ns/message**
with no fixed component at all. So the entire fixed cost is the book's
pools being faulted in and grown, and at 800,000 messages roughly 70% of
the runtime is that.

Every A/B in this file was run at 800,000 messages. Which means every one
of them compared two builds in a regime where the signal was diluted by a
constant ~70% overhead — and a real 10% improvement in the per-message cost
would have appeared as ~3%.

`hft_stage_bench` does not have this problem: it caps live orders at the
level count, so its book is bounded and its throughput is flat across a 16x
size range — 2.99M, 2.86M and 2.87M msg/s at 400k, 1.6M and 6.4M messages.
**That is an instrument. `hft_bench` is not**, and it should not be used to
judge engine speed.

The prefetch experiment was re-run on the correct instrument and stayed
refuted (−0.92 pp, 5 of 13 pairs, p = 0.29), which is the useful
confirmation: the refutations survive an instrument this noisy, even
though one earlier run at 3.2M messages had briefly shown p = 0.046 and a
*confirmation* run at the same size had shown −1.10 pp, p = 0.50. Two runs
of the same comparison disagreeing in sign is what a 5% effect inside a 6%
noise band looks like.

## The dense price ladder: a tail fix, not an average fix

Research first, because this had already been reinvented badly. The
structure every low-latency book converges on is a **direct-indexed price
grid with a per-side occupancy bitmap** — not a hash map keyed by price and
not a sorted linked list:

> "Bid/ask **dense price ladders** → price-level aggregate quantity and
> order count → intrusive FIFO queue of live order nodes. OrderId hash
> table → live order node → price-level queue links. **The price grid
> determines a direct ladder index.** The book validates that its number of
> ticks is within a named dense-ladder bound. Each side maintains an
> **occupancy bitmap so the best ask and best bid can be located without
> scanning order nodes.**" — Kalshi, ADR-003

And the documented pitfall, which is why the bitmap is not optional: the
pure-array variant "will give O(1) always for add operations, but at the
cost of making deletion/execution of the last order at the inside limit
O(M)" unless the best level is tracked incrementally.

Implemented as `LadderConfig`, opt-in, with the hash map and linked list
retained as the out-of-band fallback — so a sparse instrument keeps
working and a liquid one never touches the fallback. The bitmap handles
the documented pitfall: best-bid is the highest set bit, best-ask the
lowest.

### It removes the worst case completely

`hft_ladder_bench` forces a walk of N levels by inserting at rank N:

| depth | walk = depth | sparse | dense |
|---|---|---|---|
| 1,000 | full ladder | 39.69x | **1.01x** |
| 3,200 | full ladder | 177.63x | **0.93x** |

Flat across every distance at every depth: the O(depth) walk is gone. In
absolute terms the deepest row went from 28,324 ticks/op to 155 — **183x**.

### And it costs 2.5% on the average

End-to-end on a realistic bounded book (`hft_stage_bench` deep, 1.6M
records, medians of 7): sparse **2,983,961** msg/s, dense **2,908,714**
msg/s, **−2.5%**.

Both numbers are true and the difference between them is the whole point.
A real price process inserts near the touch most of the time, so the
*sparse* book's average walk is short — and eliminating a short walk saves
little. The dense ladder's value is that the 177x case cannot happen at
all. **This is a tail optimisation, and whether you want it depends on
whether your latency budget cares about p999.** In a market maker it does.

The 2.5% is not free either: `ladder_slot` divides by the tick, an integer
division by a runtime value at roughly 30 cycles, which the sparse book
never paid. Threading the slot through the add path cut that from three
divisions to one. Replacing it with a precomputed reciprocal is the
obvious next step and was not attempted here, because an off-by-one in a
price-to-slot mapping is a silent wrong-price bug and this path has already
produced enough of those.

### Two bugs this change introduced, both caught by the differential

The dense ladder is the first thing in this repository implemented
knowingly rather than derived, and it immediately produced two defects that
450,000 differential operations caught and review did not:

- **`bid_bits_` declared `uint32_t` instead of `uint64_t`.** A narrower word
  silently truncates `1ULL << (slot & 63)`, setting the wrong bit for every
  slot whose bit index exceeds 31 — half of them. It would have surfaced
  as `best_bid` returning the wrong price. `-Wconversion` caught it at
  compile time, which is the only reason it was caught at all.
- **The best-word hint declared "empty" instead of rescanning.** When the
  hinted word emptied on a clear, the hint was set to its sentinel — losing
  every level in every other word. Four of five seeds caught it; the fifth
  took a seventh operation to trip.

Both are in `docs/BUGS.md` alongside the FlatMap wedge, because the pattern
is the same one this repository keeps rediscovering: **a cached index that
is not invalidated correctly is worse than no cache**, and the only thing
that reliably catches it is an independent implementation to compare
against.

### Why the ladder was not the whole story anyway

Even with the walk eliminated, end-to-end barely moves, because on a
realistic feed the walk was never the cost. The average case was already
dominated by the order-reference hash and the pool accesses. That is
consistent with everything else measured here: this workload resists
improvements that make existing accesses cheaper and rewards only ones that
remove them.

## Where the time actually goes

`hft_bench decode` splits the per-message cost:

```
hft_bench 800000 decode        26,104,293 msg/s     38 ns/message
hft_bench 800000 throughput     3,242,697 msg/s    308 ns/message
```

**Decoding is 12% of the cost. `OrderBook::add` is the other 88%** — about
270 ns at 800,000 messages, though note the caveat above: that figure is
inflated by pool warm-up, and on the bounded-book instrument the whole
decode-apply-decision-risk pipeline runs at ~348 ns/message.

That is a very large number for what the operation does: two hash probes,
a pool pop, a few field writes and a linked-list append. It is large
because the operation performs about **four random accesses into
structures of 26–38 MiB**, and each one that misses L3 costs a DRAM round
trip:

| Access | Target | Size |
|---|---|---|
| `order_index_` probe | 2M-slot table | 26 MiB |
| `level_index_` probe | 2M-slot table | 26 MiB |
| `levels_[level]` | level pool | 38 MiB |
| `orders_[lv.tail].next` | order pool | 38 MiB |

Four misses at ~70–80 ns each accounts for very nearly all of the 270 ns.

### Why the optimisations failed, precisely

This is the part worth keeping, because each failure was predicted by a
specific wrong assumption:

- **Cache-line interleaving** assumed three lines per probe was the cost.
  The three loads are *independent*, so they issue in parallel and expose
  roughly the latency of one. Reducing three concurrent misses to one
  reduces bandwidth, not latency exposure.
- **The single-probe collapse** assumed the two probes to the same
  address were separated by long enough for the line to be evicted. They
  are not: the intervening work is a few dozen cycles, so the second
  probe was already an L1 hit and there was nothing to save.
- **Lookahead prefetching** assumed the misses were latency-exposed with
  nothing overlapping them. The out-of-order engine was already overlapping
  them, and the two accesses that dominate — `levels_[level]` and
  `orders_[lv.tail]` — **cannot be prefetched at all**, because neither
  address is knowable until after the hash probes have completed.

So the constraint is not latency exposure, not layout, and not footprint.
It is simply **the number of random memory accesses per operation**, and
only a change that reduces that count can help.

### What would actually work, in order of expected value

1. **Co-locate a level with its orders.** `levels_[level]` and
   `orders_[lv.tail]` are two misses into two different 38 MiB arrays for
   one logical operation. Putting the level header adjacent to its order
   list, or caching the tail `OrderNode` inline in the level, collapses two
   misses into one. This is the single biggest available win and it is a
   data-structure change, not a tuning knob.
2. **A per-level ring buffer instead of an intrusive linked list.**
   Appending becomes a sequential write into a contiguous per-level array
   instead of a random one. Removal from the middle is the hard part, and
   for a market maker's own quoting orders it is rarely needed — which is
   exactly the workload this repository models.
3. **Direct-index the order reference.** ITCH references are day-unique.
   Where they are dense enough, a direct-mapped array removes both the hash
   computation and the probe-length uncertainty.
4. **PGO and LTO**, on a toolchain that has them (both unavailable here —
   see below).

Items 1 and 2 are the work. Everything attempted in this session was
item 5: making existing accesses cheaper, which is the one category
measured to be worthless on this workload.

## How anything here was measured

`scripts/ab.ps1` runs `hft_bench` in pairs and compares **within** each
pair. Three properties of the harness matter more than the numbers:

- **Throughput mode, not the instrumented loop.** `hft_bench`'s
  instrumented loop takes three `QueryPerformanceCounter` reads per
  message. See "the benchmark was measuring itself" below — that
  instrumentation was 26% of the reported throughput, so judging an
  optimisation against it dilutes every result by the same constant.
- **Median and MAD, never mean and standard deviation.** Seven identical
  runs of the same binary have spanned 17% raw on this host, with
  occasional runs 12% below the cluster. A mean is moved by one of those;
  a standard deviation is inflated by the square of its distance.
- **Paired, interleaved runs.** Run-to-run spread on this box is 6–8%,
  which is larger than most effects worth having. Comparing A and B
  seconds apart makes machine drift common-mode, so it cancels in the
  ratio.

The verdict separates two different questions: **which** build is faster,
and **by how much**. A sign test over the pairs answers the first far more
reliably than any spread-derived threshold answers the second, so both are
reported. `IMPROVEMENT -- direction significant, size inside noise` is a
real result, not a hedge.

Invariants checked after every change: 17/17 CTest, 783 unit checks plus
risk/OMS/strategy/concurrency/sharding, and the deterministic book
checksum unchanged at `90ef6cd725f4dc11` across all six optimisation
levels.

---

## The one that mattered: the benchmark was measuring itself

**+25.8%.** This is not an optimisation of the engine; it is the removal of
a measurement artefact that was being counted as engine cost.

`hft_bench`'s per-message loop bracketed each stage with
`Stopwatch::now_ns()` — three `QueryPerformanceCounter` reads per message
to populate the decode / book-update / total histograms. A QPC read costs
roughly 27 ns on this host, so the instrumentation added ~81 ns to every
message. Measured throughput was ~2.53M msg/s instrumented against
~3.18M msg/s uninstrumented for identical work.

So the previously reported "344 ns per message" was roughly a quarter
clock. `hft_bench` now takes a mode argument:

```
hft_bench 800000 latency      # per-stage histograms, 3 clock reads per message
hft_bench 800000 throughput   # two clock reads for the entire run
```

The two loops are written separately rather than as one loop with a flag
tested inside, because a branch on a predictable flag still costs the
branch and still leaves the histogram updates in the loop body.

**This is why the first optimisation failed.** See below.

## What was tried, and what actually happened

| Change | Result | Kept? |
|---|---|---|
| Remove per-message instrumentation | **+25.8%** | yes |
| `-march=native` | **+4.4%**, 13/15 pairs, p = 0.0037 | as an option |
| Interleave `FlatMap` entry, 3 arrays → 1 cache line | +0.5% / −1.2% | **no** |
| Single-probe `add` (drop `contains`+`insert`) | +1.1 pp, p = 0.30 | **no** |
| Lookahead prefetch of the next add | +2.8 pp, p = 0.15 | **no** |
| **Dense price ladder + occupancy bitmap** | **177.63x → 0.93x** worst case; **−2.5%** average | yes, opt-in |
| Shrink `OrderNode` 56 → 48 bytes | +0.8 pp, p = 0.30 | yes, as dead-state removal |
| Size the level index to a realistic book | not measurable — see below | no |
| LTO | unavailable | n/a |
| PGO (GCC and clang) | unavailable | n/a |
| FlatMap load factor 0.5 → 0.75 | no-op by construction | n/a |

### `-march=native`: +4.4%, direction certain

Measured paired against the same source at portable ISA: 13 of 15 pairs
favour the native build, median paired delta +4.38 percentage points, sign
test p = 0.0037. A second independent run gave 14/15, p = 0.0005, median
+5.19pp. The direction is beyond doubt; the magnitude is somewhere around
4–5% with wide uncertainty, because the paired spread is ~4.7pp.

**Not made the default.** `CMakeLists.txt` already argues this: an
optimised-for-this-host binary produces numbers that are not comparable
across machines, and a committed default that is right on the author's
laptop and wrong on the reviewer's is worse than a portable one. The
option exists, the win is measured, and the benchmark host — where the ISA
is pinned anyway — should be built with `HFT_NATIVE_ARCH=ON`.

### Interleaving the `FlatMap` entry: refuted

The hypothesis was specific and, on its face, well founded. `FlatMap`
stored its state, key and value in **three parallel vectors**, so a single
probe read `slots_[i]`, `keys_[i]` and `values_[i]` — three arrays at three
addresses, hence three cache lines. Interleaving them into one
`{K key; V value; Slot state;}` entry gives exactly 16 bytes for the
(uint64, uint32) instantiation the order book uses, which is four entries
per 64-byte line: one line per probe instead of three.

At the time, the working set was ~157 MiB against 8 MiB of L3, and ingest
ran at ~344 ns/message — about 1,100 cycles, which is DRAM latency
accounting for very nearly all of it. "Three cache lines per probe" looked
like the whole story.

**It was not.** Measured −1.15% paired, and +0.51% on the earlier
unpaired run. Both inside the noise. The change was reverted.

The reason is worth keeping, because it is the kind of thing that is easy
to be wrong about confidently: **the three loads in a probe are
independent addresses, so they issue in parallel.** Memory-level
parallelism means three concurrent misses expose roughly the same *latency*
as one, while costing three times the bandwidth. This workload is
latency-bound rather than bandwidth-bound, so trading three parallel misses
for one buys nothing on the critical path. Reducing lines helps when the
loads are *dependent* — you cannot overlap a load with the address it
feeds.

### Shrinking `OrderNode`: kept, but not as a performance claim

`OrderNode` carried an `original_size` field that was written on every add
and read by nothing — 8 bytes per order in the one structure every single
add touches. Removing it takes the node from 56 to 48 bytes and the pool
from 45 MiB to 38 MiB.

Throughput effect: **+0.8pp, 9 of 15 pairs, p = 0.30. Nothing.** A 14%
smaller pool changes nothing when the pool was already three times the
size of the last-level cache; it is still one DRAM round trip per random
access either way.

Kept anyway, and the distinction matters: this is a *dead-state removal*,
justified on its own terms, that happens to be performance-neutral. It is
not a speed-up and is not claimed as one.

### LTO and PGO: unavailable on this toolchain

Worth recording so nobody re-derives it:

- **GCC 16.2.0 (w64devkit) has LTO disabled.** `-flto` fails with
  `LTO support has not been enabled in this configuration`, and
  `-ffat-lto-objects` does not help.
- **GCC PGO is broken in the same build.** `-ftest-coverage` emits `.gcno`
  correctly, but `-fprofile-generate` and even
  `-fprofile-arcs -fprofile-values` emit nothing at all.
- **clang 23.1.2 has no compiler-rt.** `-fprofile-instr-generate` fails to
  link: `libclang_rt.profile.a` is not present, and no `libclang_rt.*`
  ships in the package.

These are all *toolchain* limitations, not code ones, and all three are
routinely worth 10–20% on a latency project. They belong on the benchmark
host with a full GCC or clang toolchain. Any latency table produced before
that run should be read as "PGO and LTO not applied".

### A planned change that turned out to be a no-op by construction

Lowering `FlatMap`'s load factor from 0.5 to 0.75 was on the list as a way
to halve table memory. It does nothing for this workload, and never could:
capacity is a power of two, so for 800,000 entries both `2 x n` and
`1.34 x n` round up to 2,097,152 slots. Power-of-two quantisation means a
load-factor change only pays on sizes that happen to straddle a doubling.
Noted so it is not attempted again.

---

## What this says about the engine

The honest summary is that **the engine was never the bottleneck — the
measurement of it was.** Roughly a quarter of the reported cost was the
clock reading the clock, and the two structural changes tried after fixing
that both landed inside the noise.

The remaining ~950 cycles per message are dominated by the *count* of
dependent DRAM accesses, not by cache-line count, footprint, or
instruction selection. The next things that would plausibly move it are
therefore:

1. **Fewer dependent memory round trips**, not better layout. The order
   index is probed twice per add (`contains`, then `insert`); collapsing
   that to one probe is the obvious candidate and was not reached here.
   Note the shape of the argument: it removes an access rather than making
   an existing one cheaper, which is the only kind that has helped.
2. **Prefetching the next message's index slot.** The feed is a contiguous
   buffer with sequential order references, so the slot is computable one
   message ahead. This attacks latency directly, which is the actual
   constraint.
3. **PGO and LTO**, on a toolchain that has them.

None of these can be evaluated properly on a consumer desktop whose
run-to-run spread is 6–8% and whose clock cannot resolve a single-digit
nanosecond. The development host is good enough to *refute* a change, as it
just did twice. It is not good enough to confirm a 5% one, which is why
the benchmark host matters more than any of the code above.

## Reproducing

```powershell
# Baseline for this host
scripts/ab.ps1 -Label baseline -Runs 11 -Save

# Paired comparison of two builds
scripts/ab.ps1 -Label some-change -CompareBuildDir build-alt -Runs 15

# List what has been recorded
scripts/ab.ps1 -Label x -List
```

Not for publication. See [`ENVIRONMENT.md`](ENVIRONMENT.md).
# Bugs this repository found in itself

Every entry here is a defect that was **found, fixed, and is now guarded
by something that fails when it returns**. They are collected in one
document because the catalogue is the strongest evidence in this
repository — a claim that a system is correct is worth very little, and a
documented list of what it got wrong, with the mechanism each time, is
worth a great deal.

None of the code in `include/` or `src/` knows this file exists. The bugs
live in the working tree; the write-ups live here; `README.md` keeps only
the two that a reader has to see before trusting anything else.

## How to read this

The entries are grouped by **how the bug was found**, because that is
the transferable part. A defect found by a unit test is uninteresting on its
own. A defect that survived every green check this repository could produce
teaches you something about the shape of the problem.

| Found by | Entries | What it says |
|---|---|---|
| A unit or differential test | 14 | Ordinary. The tests earn their keep here. |
| Code review before running | 1 | Cheapest possible time to find it. |
| A second compiler | 2 | One compiler's warnings are not the project's warnings. |
| **Comparing against the specification** | **4** | The only method that finds a wrong-but-self-consistent system. |
| **Reading this repository's own output critically** | **5** | The most dangerous bugs produce good-looking numbers. |

The last two rows are the ones worth memorising, and they are the reason
the first three exist. A test suite can only prove that two things agree.
If the thing under test and the thing it is compared against share an
assumption, they will agree perfectly and both be wrong.

---

## The two headline bugs

Both are linked from the top of `README.md` because both are the kind of
thing that ends a career if it reaches production, and both were invisible
to everything this project had built up to that point.

1. **The Add Order decoder read the share count as the price.**
   [`README.md`](../README.md#the-bug-that-mattered) — 512 checks at the
   time, a
   400,000-operation differential test and a determinism harness all
   passed, because the generator wrote the same wrong layout the decoder
   read. Both agreed with each other and both disagreed with Nasdaq's
   published field table.
2. **Every latency figure was wrong by a factor of 100.** The benchmarks
   recorded `QueryPerformanceCounter` ticks into histograms whose output is
   presented as nanoseconds, and on this host QPC runs at 10 MHz.
   [Phase 8](#phase-8-every-latency-figure-in-this-repository-was-wrong-by-100x)

They are the same species of defect. In both cases a value was produced by
a transformation that was internally consistent and wrong at the boundary,
and in both cases nothing in the repository was capable of noticing because
nothing in the repository was external to the mistake.

---

### A third bug class: the dense ladder's own two

The dense price ladder is the first thing in this repository implemented
because the literature said to, rather than derived. It immediately
produced two defects that 450,000 differential operations caught and
review did not.

**A bitmap word declared 32 bits instead of 64.** `bid_bits_` was
`std::vector<std::uint32_t>`. A narrower word silently truncates
`1ULL << (slot & 63)`, setting the wrong bit for every slot whose bit index
exceeds 31 — half of them. The symptom would be `best_bid()` returning the
wrong price, intermittently, on roughly half of all levels.
`-Wconversion` caught it at compile time, which is the only reason it was
caught at all.

**A cache hint that declared "empty" instead of rescanning.** Best-bid is
found by scanning for the highest set bit, which is O(band/64) and made the
dense ladder **2.6× slower** than the hash map it replaced. The fix was a
cached best-word index — and the first version of that cache, on clearing
the hinted word, set the hint to "nothing occupied" rather than scanning for
the next occupied word. Every level in every other word was lost.
Four of five differential seeds caught it; the fifth took seven operations.

The pattern is the same one this catalogue keeps arriving at, which is why
it is worth naming a third time: **a cached index that is not invalidated
correctly is worse than no cache**, because it converts a slow correct
answer into a fast wrong one. Nothing but an independent implementation to
compare against finds it.

## The worst bug in this repository: the order book wedges permanently

Found while writing `hft_ladder_bench`, and it is the most serious defect
this project has ever contained. It was present for the project's entire
life, it passed every test, and on a live feed it would have stopped the
book accepting orders.

**The mechanism.** `FlatMap::insert` recorded the first tombstone in a
key's probe run and then *waited for an `empty` slot* before using it:

```cpp
if (slots_[slot] == Slot::tombstone) {
    if (first_free == kNoHandle) { first_free = slot; }
    continue;                       // ...and keep looking for an empty
}
// Slot::empty: only NOW is first_free used
return place(key, value, (first_free != kNoHandle) ? first_free : slot);
```

The reasoning was reasonable — an empty slot ends the probe chain cleanly,
so preferring it keeps the distribution good. It is also wrong the moment a
probe run contains tombstones and occupied slots but **no empty slot**. Then
the loop wraps the entire table, finds nothing, and returns `false` — while
`first_free` is sitting there holding a perfectly usable slot.

**Why it is a permanent wedge and not a slowdown.** `OrderBook` erases from
`order_index_` and `level_index_` on every removal, so both tables
accumulate tombstones as the book churns. Once every slot is
occupied-or-tombstone:

- the next add of a new order reference returns `capacity_exhausted`;
- every subsequent add does too, **forever**, because the tombstones are
  never reclaimed in bulk and live occupancy is a small fraction of
  capacity.

A book that had traded a few hundred million shares would stop accepting
orders while looking nearly empty. There is no degradation curve, no error
rate to watch, and no recovery short of restarting the process.

**How it was found.** `hft_ladder_bench` sizes the book's capacity to the
working set, which no other tool does. Every other test sizes capacity far
*above* its operation count — the differential test drives 200,000
operations through a book with 1,048,576 order slots — so cumulative churn
never once reaches capacity and the bug is unreachable. The ladder benchmark
sizes tightly, added 20,000 orders, and got `capacity_exhausted` on the
first one. A minimal reproducer fails at exactly cycle 256 with 16 slots.

**The lesson, which is about the tests rather than the code.** Every test in
this repository was correctly written and all of them shared one
assumption: *the number of operations is smaller than the capacity*. That
assumption was never written down, never asserted, and never tested, so
nothing noticed it was load-bearing. A property that holds only while a
parameter stays in a range is a property that should be tested at the edge
of that range.

**The fix.** Keep scanning for an empty slot to preserve the distribution,
but treat a remembered tombstone as a valid destination in its own right
and use it when the run ends without one. `insert` now returns `false` only
when there is no empty slot *and* no tombstone anywhere in the run — the
only case where the table genuinely cannot hold another entry.

The regression test (`test_flatmap_churn`) asserts the fix, and
deliberately asserts the *opposite* failure too: that a genuinely full table
still reports full. Fixing a capacity bug by making `insert` always succeed
would have traded a wedge for a silent unbounded map, which is worse.

The fix changed no book state: the deterministic checksum is identical
before and after (`90ef6cd725f4dc11`), because which slot a key lands in was
never observable.

### A second finding in the same component

Fixing the wedge exposed the related performance cliff, which is a real
limitation rather than a bug. `insert` probes forward until it finds an
`empty` slot, skipping tombstones. So a table that is mostly tombstones has
long probe chains even when nearly empty.

`hft_ladder_bench` measured this accidentally, by sizing capacity at 4× the
working set: add/remove cost came out at ~900 ns per operation, scaling with
*depth* and flat in *walk distance* — nothing like a ladder walk and entirely
explained by probe length. Real fix is to purge tombstones when they exceed
a fraction of capacity, which needs a rehash and therefore an allocation
this design does not permit on the hot path. Recorded here rather than
fixed, because the honest answer is that it is a trade-off, not an oversight,
and the right trade-off depends on a capacity policy this project has not
settled.

---

## The market maker's feed is the second half of this

With the decoder fixed, the market maker's numbers collapsed: volatility
exactly zero, quoted half-spread a third of a tick, and therefore a
strategy that quoted nothing at all.

The cause was not the decoder. It was that the book genuinely did not
move. The feed used to carry 24 live orders across 4 price levels a side
with a mean reversion of 1/64 per record, and under those settings the
touch is never left empty, so the best bid and ask are set early and
frozen. The mid moved on **0.7% of observations**. More than half the
per-tick returns were exactly zero, so the median-absolute-deviation
volatility estimate was zero, so the Avellaneda-Stoikov risk term was
zero, so the spread collapsed below the one tick that can be placed.

This was already documented as a known failure mode — "a price that does
not move is not a market, and a strategy cannot be studied on one" — and
it had been fixed once before by bounding the live-order count. The
decoder bug then re-created it, because a book whose "prices" were random
share counts *did* appear to move. The old measurement was measuring the
misdecoded field.

The fix is the same lever applied again: 6 live orders over 4 levels a
side, and reversion of 4 instead of 64, which lets the walk travel. The
mid now moves on 4.3% of observations, sigma lands near 43 raw units per
tick, and the quoted half-spread is about 2.8 ticks — set by the risk
term rather than pinned down by the tick grid, which is the regime where
the model is actually doing something.

Two things are now permanent rather than incidental:

- **`hft_market_maker` prints how often the mid moved, before any other
  number.** A mid that barely moves makes volatility, spread, markout and
  PnL meaningless rather than merely wrong, and a reader should not have
  to know that to distrust them.
- **The tool warns below 2%**, so this cannot recur silently.

**Bugs the tests found, kept here deliberately.** These are the
reasons the tests exist and the reasons to distrust code that has never
been differentially tested:

- A partial fill was implemented as unlink-then-relink, which moved the
  order to the tail of its price level. A partial fill must not change
  queue priority. Caught by the first partial fill in the run.
- Full fills and full cancels zeroed the order size *before*
  detaching, so the level aggregate subtracted zero and was left
  permanently inflated. Caught 37 operations in.
- The level index entry was erased unconditionally on removal, so
  removing one of two orders at the same price made a still-populated
  level unreachable. Found by review before it ever ran.
- The ITCH Order Cancel and Order Delete body sizes were hand-written
  as 20 and 16 and were wrong by 3 bytes each; the `static_assert`s did
  not catch it because they compared constants to each other rather
  than to the field offsets. The sizes are now derived from the offsets.
- The feed generator indexed one past the end of its live-order vector
  whenever exactly one order was resting. Heap corruption, surfacing
  only as tens of thousands of undecodable records in the replay tool.
- The feed generator built cancel, execute and delete frames and then
  discarded them without emitting, while still consuming a sequence
  number for each. The SOUP sequence check reported 79,707 missing
  messages, which is exactly what it exists to report.

The last two are worth dwelling on. Both were in the *test
infrastructure*, both were found only because a check that should have
been automatic actually ran, and both would have been invisible to
anyone reading the book code alone.

Phase 3 added four more, and the pattern is worth naming because it is
the same lesson three times over:

- The `TokenBucket` treated its burst multiplier as an absolute token
  count, so a 100/second limit admitted exactly **one** order and then
  refused ninety-nine. The rate limiter worked; it simply did not work
  at the rate anyone asked for.
- The refill clamped the interval to the time to add *one* token rather
  than to fill the *bucket*, silently truncating any interval longer
  than a fraction of a second.
- The refill then divided by 1000 in the wrong direction, granting
  1/1000th of the tokens owed: 0.005 tokens per half second instead of
  five.
- The OMS state counters were decremented on slot reuse even for a
  never-used slot, and the `pending_new` count was never incremented,
  so a reconcile view reported **-105 orders in live states** after a
  randomised run.

A rate limiter that quietly admits one order per second, and a
reconciliation counter that goes negative, are both the kind of defect
that reaches production. Neither is the kind that code review catches.

Phase 4 added six more, and two of them are worth reading twice:

- The feed generator chose the side and the price offset
  **independently**, so bids and asks were drawn from the same wide
  band and overlapped. The best ask sat **$15 below the best bid**. A
  crossed book cannot occur in a real market, and every measurement
  taken from one — mid, volatility, markout, PnL — is meaningless
  rather than merely wrong. It was invisible until the resting quote
  was printed next to the touch.
- The fix for that had its own bug: a single `have` flag was guarding
  **both** sides of the clamp, so the minimum ask was never recorded
  and the bid clamp never fired. The first fix appeared to work and
  did nothing.
- A book that only ever accumulates never clears a price level, so
  its mid is frozen: **24 moves in 40,000 messages**. A price that does
  not move is not a market, and a strategy cannot be studied on one.
- Avellaneda-Stoikov was implemented with a *fractional* sigma. The
  model is dimensionally incoherent that way, the inventory term
  collapses to zero, and the spread became a third of a tick — so the
  strategy quoted nothing at all, silently.
- The markout was recorded against the mid **after** the move that
  filled it, which prices every fill as better than the mid and
  reports a negative effective spread. This is the exact bug the
  metrics header warns about, committed in the same commit.
- `realisation_ratio` was reported as a finding while being
  meaningless: for a market maker it is negative whenever fills are
  passive, which is always. It was replaced with the markout, which is
  the number that carries information.

Phases 5 and 6 added eight more. The first two are the most instructive
defects in this repository, because both produced a plausible *number*
rather than a failure, and one of them produced a plausible *explanation*
for a number that was itself wrong:

- **The round-trip benchmark used one ring in both directions.** It
  pushed a token down an SPSC queue and waited for it to come back up the
  same one. That quietly makes *both* threads consumers of a
  single-producer queue. It did not deadlock and it did not crash: it
  reported a clean, confident, roughly sixty-fold speedup that was
  measuring two threads racing on the same index. The fix is two queues,
  one per direction. The lesson generalises — a concurrency bug does not
  have to manifest as a hang, and the most dangerous ones are the ones
  that produce a good-looking number.
- **A drain loop that exited on a flag it had read too early.** The
  consumer drained the ring, found it empty, and *then* checked the
  producer's stop flag — and the producer could push more messages and
  set that flag in between. The consumer left with messages still
  queued, losing roughly a sixth of the stream. The interesting part is
  what it did next: the benchmark reported a 20% **slowdown**, and its own
  output explained that slowdown with a plausible-sounding story about
  load imbalance and the cost of hand-offs. The story was wrong; the
  number was an artefact of doing less work. It survived only because it
  fit, and it was caught only because the benchmark compares books
  against a baseline and marks a mismatch INVALID instead of reporting
  it as a result. The same defect had already been found and fixed once,
  in the concurrency test's drain helper — so it now exists once, in one
  place, with a comment explaining both failure modes.
- **A packed routing value returned unpacked.** The sharded dispatcher
  stored `(worker, local_index)` folded into one 32-bit word and then
  handed the *packed* word to the worker as the local index. Every
  mutation arrived with an index in the tens of thousands, failed the
  bounds check, and was dropped; only the Adds were ever applied. It
  looked like a successful speedup right up until the checksums
  disagreed.
- **`Symbol::c_str()` was not NUL terminated.** The ninth byte held the
  string *length*, so a `std::unordered_map<std::string, ...>` keyed on
  it read past the object on every lookup, missed every time, and
  concluded that all sixty-four symbols in a multi-symbol feed were
  distinct — so every worker claimed every book. One character, one
  access violation, and now a regression test that reads the terminator
  directly.
- **The consumer drained once and exited**, before the producer had pushed
  anything, which left the producer blocked forever on a full ring. This
  is not a rare interleaving; it is the *common* one, because a freshly
  spawned consumer usually gets scheduled first. The exit condition also
  needed one more drain pass after the stop flag: the pass that found the
  ring empty may have run before the final push landed, and skipping the
  retry silently drops the tail of the stream.
- **A ring of 1024 slots × 128-message batches is 8MB**, and it was a
  stack local. The tool died with a stack overflow two thirds of the way
  through its own output, having already printed four rows of a table
  that were therefore never seen.
- **The OMS could `retire()` a negative slot.** Every public entry point
  takes `int slot`, and `retire` took `std::size_t`, so a bad slot became
  a huge unsigned index and a write out of bounds. It was unreachable
  only because `lookup` happened to reject the bad value first — a
  property of the call order, not of the callee.
- **`__int128` under `-Wpedantic`.** The overflow-safe notional multiply
  is the right way to do it and `__int128` is not ISO C++, so the
  project built with clang and failed with GCC. The relaxation is now
  scoped to those four lines rather than applied to the translation unit,
  because widening a pedantic setting project-wide to accommodate one
  extension is how a second extension gets added unnoticed.

The last four are the ordinary kind: found by a test, a crash, or a
compiler. The first is the kind worth remembering.

And the one above all of them is not in this list because no test found
it. It took reading the published field table against the code, which is
a check this repository had never performed on itself.

### Phase 8: every latency figure in this repository was wrong by 100x

The second bug found by no test, and it is in the measurement layer rather
than the engine. It is documented at length because the mechanism is more
interesting than the fix.

`hft::util::Timer::now()` returns the platform's native ticks. On POSIX
those are already nanoseconds. **On Windows they are QueryPerformanceCounter
ticks, and QPC ticks are not nanoseconds.** On the development host
`QueryPerformanceFrequency` returns 10 MHz, so one tick is 100 ns.

Every benchmark recorded `Timer::now()` deltas straight into a
`LatencyHistogram` and printed the result in columns presented as
nanoseconds. So a reported `decode p50` of 2 was 200 ns, and the
`clock read p50` of 1 was 100 ns.

**Throughput was correct**, and that is the part worth studying. Throughput
was the one figure computed through `Timer::elapsed_ns()`, which converts.
It was the only latency-derived number with an independent quantity to be
checked against — the wall-clock time the run actually took — and it was
the only one that was right. The unconverted figures had nothing to check
them against, so nothing did.

Three things are now true that were not:

1. **`Timer::now_ns()` exists and every stage boundary uses it.** The
   conversion happens at the point of capture rather than at the point of
   recording, so a `record()` call receives nanoseconds by construction.
   The benchmarks pass through `bench::Stopwatch` rather than calling the
   raw clock, because five call sites each remembering a conversion is five
   chances to forget it again.
2. **The clock frequency is printed in every benchmark's banner.** The
   number every latency figure depends on is not 1 GHz everywhere, and it
   used to appear nowhere.
3. **`test_timer_units` asserts a measured 30 ms interval converts to
   millions of nanoseconds and not thousands.** A conversion checked only
   against its own definition passes just as happily when the definition is
   wrong. This one is checked against a duration the test knows
   independently.

The generalised lesson, and it is the same lesson as the Add Order bug one
section higher up: **a value read from a clock is not a duration, and a
duration is not a number you may print without saying what it is in.** Two
independent clocks in this repository disagreed about a unit — one
reporting ticks as nanoseconds, the other dividing a TSC delta by a QPC
delta and calling the result ticks per second — and in both cases the
arithmetic was internally consistent and the answer was wrong by exactly
the ratio between the two clocks.

That second instance is the more instructive of the two, because it is the
one that *looked* validated. The TSC calibration divides a TSC delta by an
interval measured by a different clock, and it was dividing **platform
ticks by platform ticks** and multiplying by 1e9. The result was a
ticks-per-second figure of 320 **GHz** on a 3.2 GHz part — a number with a
plausible shape, the right order of magnitude for a modern CPU, and wrong
by a factor of exactly 100. It survived review because nothing about
"320 GHz" looks like a unit error.

The same class of mistake then appeared a third time inside the new
calibration code, where a 20 ms window deadline was computed by adding a
nanosecond constant to a tick-valued timestamp. Every "20 ms" window was
really two seconds, and the tool took 29 seconds instead of one — while
still printing a plausible calibration, because the *ratio* it computed
stayed self-consistent even though the *interval* was wrong.

Three instances of one mistake is not carelessness, it is a design
failure, so the fix is mechanical rather than careful:

- **A reference-clock value is converted at the point of capture.** Inside
  `TsClock`, the only reference-clock accessor used anywhere is
  `now_ns()`. There is no tick-valued timestamp left in that file to
  confuse with a nanosecond constant.
- **`hft_tsc_bench` verifies its own calibration window** and exits
  non-zero if a 20 ms window is not between 15 ms and 60 ms. A wrong window
  length means a conversion is being applied across an interval nobody
  intended, and the tool now refuses to print numbers derived from it.
- **The tool cross-checks TSC against QPC over one shared interval** and
  warns above 0.5% disagreement. The two clocks are independent, so their
  agreement is a real test rather than a tautology. It currently reports
  0.0001% on a 3.2 GHz part.

The calibration now also states the limit it will not cross: **on a modern
x86-64 CPU with an invariant TSC, ticks are not cycles.** A core running
above its nominal frequency accumulates ticks at the nominal rate, so
converting a tick delta into a cycle count requires APERF/MPERF or
`perf_event_open`. This repository reports nanoseconds and does not claim
cycle counts, because the honest nanosecond is available everywhere and the
honest cycle count is not.

### What the floor turned out to be

`hft_tsc_bench` exists to answer one question: when a pipeline stage
reports the same p50 as the clock read measuring it, is the stage fast or
unmeasured? `results/ENVIRONMENT.md` had recorded a decode p50 equal to
the clock-read p50 and correctly declined to call it free, but nothing
measured the floor, so nobody else could check the claim.

Measured on the development host:

| Clock                    | Pair overhead (p50) | Floor      |
|--------------------------|---------------------|------------|
| QPC (`QueryPerformanceCounter`) | 1 tick = **100 ns** | 100 ns  |
| `rdtsc` + `lfence`       | 97 ticks = ~30 ns    | ~30 ns      |
| `rdtscp` + `lfence`      | 97 ticks = ~30 ns    | ~30 ns      |

So the honest reading of the corrected `hft_bench` table on this host is
that **decode is not measurable at all**: its p50 is exactly one QPC tick,
which is the floor, not a measurement. That was true before this fix too —
it was simply being reported as a 2 ns stage, which is 100x wrong *and*
wrong in the flattering direction.

Two further facts the tool establishes, both of which qualify every other
number in this repository:

- **The TSC is invariant on this host** — two physical cores agree to six
  figures — so a migrating thread's reading is trustworthy here. That is a
  property of this CPU, and `TsClock::invariant()` reports it rather than
  assuming it.
- **Run-to-run drift is 14% on an L1-resident workload and 23% on a
  DRAM-resident one**, measured over matched ~50 ms windows. Any ratio
  smaller than the relevant figure is noise on this host. The two regimes
  are reported separately because a memory-bound workload contends for the
  memory controller with everything else on the machine, and using one
  figure to qualify the other is wrong in both directions.

The tool also caught a flaw in its own methodology, which is the part
that justifies its existence. Its first drift measurement used a fixed
500,000 iterations for both workloads. A 64-byte copy costs ~22 ticks and a
4 KiB copy ~340, so the L1 run spanned 3.4 ms — shorter than a scheduler
timeslice — and reported 33% drift while the DRAM run reported 9%. The
counterintuitive result was not about caches; it was about measuring one
workload for fifteen times less long than the other. Both are now measured
over matched windows chosen from a pilot batch.

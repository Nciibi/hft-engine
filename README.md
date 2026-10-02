# HFT Engine

A from-scratch NASDAQ TotalView-ITCH 5.0 order book and market-making
engine in C++20. Fixed-point, arena-backed, zero allocation on the hot
path, deterministic replay, benchmarked to p999 across the full
decode-to-encode pipeline.

> **Status:** reference implementation built for understanding exchange
> mechanics, not for production deployment. See
> [What is implemented](#what-is-implemented) and
> [What this is not](#what-this-is-not).

## What is implemented

| Component | State |
|---|---|
| Fixed-point `Price`, no float constructor | done |
| ITCH decode: `A`, `E`, `C`, `X`, `D` | done |
| Split 48-bit timestamp reassembly | done |
| Unknown-type skip by length, truncation reporting | done |
| SOUP sequence tracking, gap and duplicate detection | done |
| Price-time priority book, slab arena, no hot-path allocation | done |
| `execute` / `cancel_partial` / `remove`, `X` vs `D` distinct | done |
| Apply layer, single dispatch point from message to book | done |
| Deterministic replay, FNV-1a book-state checksum | done |
| Pre-trade risk: position, gross, notional, price band, rate, kill switch | done |
| OMS: order state machine, slot pool, reconcile counters | done |
| Avellaneda-Stoikov quoting, no transcendental in the loop | done |
| Adverse selection: markout, effective/realised spread, toxicity | done |
| Order Replace (`U`) decode | deliberately **not** done, see below |
| SPSC lock-free ring buffer | done |
| Cache-line isolation, cache-line padded indices | done |
| Thread pinning and SMT topology discovery | done |
| Mutex + `condition_variable` baseline benchmark | done |
| Threaded pipeline equivalence check | done |
| Multi-shard by symbol | not started |
| MoldUDP64 packet framing and checksum | not started |

**Order Replace is skipped on purpose.** Its field table was not
verified against the published specification, and the alternative to
guessing an offset is honouring the skip-by-length path. A skipped
message is recoverable; a decoder that reads the wrong bytes is not.
This is the one place the decoder declines to be complete on purpose,
and the reason is recorded in `include/hft/itch/protocol.hpp`.

Verified: **137 unit + 156 risk/OMS + 47 strategy + 149 concurrency = 489
checks**, and a differential test comparing the fast book against an
independent naive model over **400,000 operations with full state
comparison after every one**, across five seeds. The OMS is additionally
driven through **60,000 randomised operations** with invariant checks
against an independent tally. The threaded pipeline is checked against
the single-threaded one: same feed, same order, and the final book is
fingerprinted with the same FNV-1a checksum the replay tool uses, so a
dropped or reordered message fails a test rather than showing up as a
speedup. Determinism is checked too: the same capture replayed at `-O0`,
`-O2`, `-O3`, `-Os` and `-Oz` produces an identical book checksum.

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
- Threaded pipeline equivalence: the decoder-thread/book-thread split
  builds a byte-identical book to the single-threaded loop over the same
  feed, at every batch size. Asserted in CI, not merely measured.

### Concurrency

Measured on `<instance spec>`, `<compiler + flags>`, `<kernel>`, with the
decoder and book threads on two distinct physical cores. Reproduce with
`./build/hft_ring_bench` and `./build/hft_pipeline_bench`.

| Variant                                     | msgs/sec | vs 1 thread |
|---------------------------------------------|----------|-------------|
| Single thread, decode + apply               | `[MEASURED]` | 1.00x   |
| Two threads through the ring, K=1           | `[MEASURED]` | `[MEASURED]` |
| Two threads through the ring, K=8           | `[MEASURED]` | `[MEASURED]` |
| Two threads through the ring, K=32          | `[MEASURED]` | `[MEASURED]` |
| Two threads through the ring, K=128         | `[MEASURED]` | `[MEASURED]` |

**The finding is negative and it is the interesting part.** Threading this
pipeline does not pay, and batching does not rescue it. Moving the
*cheap* half of the work to a second core leaves the expensive half
running serially on one core while adding a hand-off and a second
runnable thread to pay for. Larger batches make it slightly worse again,
for an independent reason: a K=128 push is an 8KB copy, which is more
cache traffic than the transfers it amortises.

What follows from that is not "batching does not work". It is that
batching a pipeline whose halves are *unequal* cannot work, and that the
design this argues for is sharding **by symbol**, so each thread owns a
book and runs a full decode-and-apply for its own instruments with
genuinely equal work. That is the multi-shard design this repository does
not have yet, and it is named as missing below for a measured reason
rather than a guessed one.

The tool prints the single-threaded baseline a second time at the end and
reports the drift as a noise floor. On the development machine that drift
is several percent, which is larger than most of the ratios in the table
above. Differences smaller than the noise floor are not results, and the
tool says so rather than leaving the reader to assume otherwise.

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/hft_test           # codec, values, sequence tracking
./build/hft_risk_oms       # pre-trade risk and the OMS
./build/hft_strategy       # quoting model, adverse selection, book invariant
./build/hft_concurrent     # SPSC ring, single- and two-threaded
./build/hft_differential   # fast book vs naive model, full state compare
./build/hft_replay         # deterministic replay, prints the book checksum
./build/hft_market_maker   # market maker over a replay, prints toxicity
./build/hft_bench          # latency and throughput table
./build/hft_ring_bench     # ring vs mutex baseline, throughput and round trip
./build/hft_pipeline_bench # 1 thread vs 2, batch-size sweep, checksum-matched
ctest --test-dir build     # everything
```

Zero external dependencies in the library target. CMake, a C++20
compiler, and `git`.

## Architecture

```
  capture.itch   [seq:4][len:2][ITCH body]
      |
      v
  [ ITCH decoder ]  zero-copy, big-endian, 48-bit timestamps
      |             A / E / C / X / D; others skipped by length
      v
  [ SOUP sequence ] gap and duplicate detection, 32-bit wrap safe
      |
      v
  [ Apply layer ]   single dispatch: message -> book mutation
      |
      v
  [ Order book ]    price-time priority, fixed-point, slab arena
      |
      +---> [ FNV-1a state checksum ]  deterministic replay
      |
      v
  [ Order management ] state machine, slot pool, reconcile counters
      |
      v
  [ Pre-trade risk ]  position, gross, notional, band, rate, kill
      |
      v
  [ Market maker ]   Avellaneda-Stoikov, tick-aware
      |
      +---> [ Adverse selection ]  markout, toxicity, realised spread
      |
      v
  [ PnL ]  marked to market, inventory aware

  --- optional, measured separately ---

  capture.itch
      |
      v
  [ decoder thread ]  -> [ SPSC ring ] -> [ book thread ]
                          one slot per batch
                          of decoded messages
      |
      +---> [ FNV-1a state checksum ]  compared against the
                                       single-threaded book
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

**Determinism is a feature.** Same capture, same book, same checksum, at
any optimisation level on any host. This is checked, not claimed: the
suite builds the replay tool at `-O0`, `-O2`, `-O3` and `-Os` and
compares. Without that property no benchmark is reproducible and no bug
is reproducible, and a latency number that cannot be re-derived is an
anecdote.

**Sequence gaps are detected, never absorbed.** A gap means the book is
missing orders the venue believes are resting, so continuing past one
produces a book that looks healthy and is wrong. The replay tool
reports the missing count and marks the run as not comparable with a
clean one. The tracker handles the 32-bit wrap explicitly, because
`observed == expected + 1` is correct everywhere except the one moment
it is hardest to reproduce.

**The ring's counters are monotonic, not masked and reused.** The obvious
implementation keeps `head` and `tail` as indices that wrap at capacity,
which makes `head == tail` mean the ring is both empty *and* full. Here
the counters only increase: `size` is `head - tail`, `full` is
`head - tail == capacity`, and each case is unambiguous with no reserved
slot and no auxiliary flag. The cost is one extra counter.

**Acquire and release, and nothing stronger.** A release store publishing
a slot and an acquire load observing it is exactly the edge that makes
the slot's writes visible before the index that publishes it. A
`seq_cst` fence would order those two atomics against every *other*
atomic in the program, which this queue has no business doing — it is not
synchronising anything outside itself. On x86-64 the acquire/release pair
compiles to plain loads and stores with no fence instruction at all, which
is the whole reason to use it here rather than the stronger option.

**Four cache lines for the indices, and the count is deliberate.** The
producer writes `head` and reads `tail`; the consumer does the reverse. If
those shared a line, every push would invalidate the line the consumer is
reading — pure coherence traffic achieving nothing. The two producer-
private cache indices are padded separately for the same reason: they are
written on opposite threads, and the refresh happens on the full/empty
path, which in a saturated pipeline is *every* operation. Four lines of
indices against a 1024-slot ring is well under 1% overhead against a
coherence tax of 50-100ns per message.

**The topology is discovered, never assumed.** Picking "core 0" and
"core 1" for the two threads is a guess that is wrong on a large fraction
of machines. On the development host here, siblings are laid out as
`(0,1), (2,3), (4,5)...`, so a `logical + 6` heuristic — correct for the
first-half/second-half layout — would put both threads on the same core
and halve the result while looking entirely plausible. The benchmark asks
the OS and prints the placement it actually achieved.

**A bidirectional exchange needs two rings, not one.** This one is in the
bugs section below, but the design consequence belongs here: an SPSC
queue has exactly one producer and one consumer, so a request/response
hand-off is two queues, one per direction. The benchmark models the two
directions as two named channel types rather than passing one object
twice, specifically so the mistake cannot be made quietly again.

**Time is injected, never read.** Every risk and OMS entry point that
needs the current time takes it as a parameter. A component that called
a clock internally could not be tested deterministically and could not
be compared against a reference implementation, which is the property
the rest of this repository depends on. The cost is one parameter.

**Every rejection has a specific reason.** There is no catch-all limit
verdict, because an unexplained rejection is unactionable for a trader
and undiagnosable for an operator. Limits are also evaluated in a fixed,
documented order, and the order is part of the contract: the kill
switch first, so a tripped switch stops everything regardless of what
else is true, and the price band before the rate limiter, so a
fat-fingered order is reported as a fat finger rather than being
silently rate-limited into a different and less actionable reason.

**The OMS state machine is a table, not scattered `if`s.** It is
exhaustively tested against an independently written 8×8 matrix, so a
single wrong edge cannot hide. `pending_cancel` is deliberately not
terminal: a fill can arrive while a cancel is outstanding, and treating
that as a completed cancel silently drops the residual order and leaks
inventory.

**The market maker quotes around the mid but is bounded by the touch.**
The Avellaneda-Stoikov model wants a spread and a price, and the
book is not centred on the mid the model is using. Each side is
therefore clamped to be at or outside its own touch and never to cross
it, which is what joining a queue means. The model still does the
deciding: when it wants a *wider* quote than the touch offers, the
clamp is inactive and the widening happens. A long inventory
therefore bids further away while continuing to offer, which is
inventory control expressed purely through placement.

**The tick size is a constraint the model does not get to ignore.**
When A-S produces a spread narrower than one tick, the quote is
widened to the minimum placeable spread rather than declared invalid.
The edge the model expected above the touch is edge the strategy does
not get, and that is precisely why tick size is a real constraint on
market making profitability rather than a formatting detail. Quoting
is reported as `tick_constrained` so the cost is visible.

**The metrics are defined, not approximated.** `realised = effective
- 2 * markout` holds exactly and the tool prints both sides of it
every run, because it is the one arithmetic relation a reviewer will
check. A *negative* effective spread is correct here: a passive fill
buys at the bid, which is below the mid, so `2*sign*(fill-mid)` is
negative by construction. The profit is in the round trip; what costs
money is adverse selection, which is why the reported number is the
markout and the toxicity rate.

**A negative EFFECTIVE spread is not a market maker beating the mid.**

**The price ladder is a sorted linked list, and that is a known cost.**
Inserting a price that is not adjacent to the best walks from the head
of the ladder, so book-update latency is proportional to ladder depth.
At the ~3,200 levels per side in the development run this is
comfortably inside the tail, but it is O(levels), not O(1), and it is
the first thing to replace with a price-ladder array or a tree once a
real depth target is known. The benchmark reports ladder depth for
exactly this reason: a latency figure quoted without it cannot be
interpreted.

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

1. **Design a low-latency market data handler.** Decoder, sequence
   tracking, and the benchmark harness.
2. **Design an OMS that stays correct under high message rates.** The
   apply layer, the order state machine, and pre-trade risk, with the
   differential test and 60,000 randomised operations as the
   correctness argument.
3. **Design a matching engine.** The book itself.
4. **How do you make money providing liquidity?** The quoting model and
   the adverse-selection measurement. This is the question engineering
   candidates most under-prepare for, and the one most likely to be
   asked regardless of the round.

Not yet built, and named honestly: sharding, the SPSC ring buffer, and
MoldUDP64 framing.

## What this is not

Stated plainly, because a reference implementation that pretends to be
production software is worse than one that does not:

- **No live exchange connectivity.** TotalView-ITCH requires a Nasdaq
  market-data agreement. This runs on captured or generated feeds.
- **No MoldUDP64 framing.** The capture format carries one sequence
  number per record; the transport's own packet header and checksum are
  not parsed. See the format note in `src/feed/generator.hpp`.
- **No Order Replace.** Deliberate, not accidental: an unverified field
  table is skipped rather than guessed.
- **No multi-shard scaling.** One instrument per book. Sharding by
  symbol needs a sequencer and a partition scheme that are not here.
- **No clock synchronisation.** No PTP, no NTP discipline, no
  cross-machine timestamp alignment. Cross-host latency claims would
  be meaningless without it.
- **No persistence or recovery.** A process restart loses all state.
  A real system needs a write-ahead log and a snapshot cadence.
- **The market making backtest is an upper bound.** No queue position,
  no latency, no size at level. Its fill model is also *symmetric*,
  which means the position is a random walk the inventory term cannot
  damp, so the strategy reaches its inventory limit and stays there.
  Real inventory control depends on fills being asymmetric, which
  requires order-flow toxicity as a model input. The adverse-selection
  metrics do not depend on this and stand on their own; the inventory
  numbers demonstrate the mechanism is wired up, not that the strategy
  controls inventory.
- **No self-trade prevention, no auction handling, no order book
  state message processing.**
- **Price ladder is a sorted linked list**, so inserting a price not
  adjacent to the best is O(ladder depth). See the design note above.
- **Benchmarks are single-socket.** No kernel bypass, no io_uring, no
  DPDK. Real shops measure the syscall layer separately because it
  dominates.

## Reference

NASDAQ TotalView-ITCH 5.0 interface specification, for the message
layouts, the big-endian field encoding, and the partial-cancel rules.
Market making follows Avellaneda-Stoikov; inventory risk handling
follows Guéant-Lehalle-Fernandez-Tapia.

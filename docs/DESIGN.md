# Design decisions

Every entry here is a decision this repository made deliberately, with the
reasoning that made it deliberate. The short form of each is in the
[README](../README.md#design-decisions); this is the long form, for when
someone asks why.

The order is roughly the order a reader meets them in the code. The
short form of each is in the [README](../README.md#design-decisions).

---

## Data representation

**Fixed-point integers, never floating point.** Prices are `int64_t` at
1/10000, matching ITCH's native precision. Floating point in a price
ladder is a rounding bug waiting for a specific fill size.

**Slab arena, no allocation on the hot path.** Orders are integer
handles into a preallocated arena, never pointers. Allocation is
batched and amortised outside the message loop. This is the same
size-class and freelist reasoning that drives
[allox](https://github.com/Nciibi/allox), my thread-cached allocator.

## Protocol correctness

**ITCH `X` and `D` are different operations.** `X` is a partial cancel:
subtract cancelled shares from the original add. `D` removes the order
entirely. Conflating them is the most common ITCH book bug, and the
naive reference model exists partly to catch it.

**Sequence gaps are detected, never absorbed.** A SOUP sequence gap
means book state is corrupt. Continuing silently produces confident
wrong numbers, which is worse than stopping.

**Unknown message types skip by length.** A live feed will always
contain types this build does not implement. Crashing is not an option.

**Wire layouts are asserted against the specification, not against each
other.** Every offset in `protocol.hpp` has a `static_assert` against its
literal value from the published field table. The relationship asserts
(`size == last offset + width`) only prove the table is internally
consistent, which is not the same as being right — and that distinction
is the entire reason the Add Order bug survived 512 green checks.
Internally-consistent-but-wrong is a real and underappreciated failure
mode: it defeats differential testing, because both sides of the
comparison share the error.

**Skip rather than guess was the right rule, and it was applied to a
message that could have been verified.** Order Replace sat on the
skip-by-length path for most of this project's life because its field
table could not be confirmed. It could be — and going to the
specification found the generator writing it four bytes too long, with
the same phantom order-entry fields that broke Add Order. An unverified
offset on a live feed produces a decoder that confidently misreads it;
a skipped message is recoverable and wrong bytes are not. The rule was
sound. The gap was that "cannot verify" had quietly become "assume it is
not knowable", which is a different claim and a weaker one.

**A clock reading is not a duration, and a duration is not a number you
may print without saying what it is in.** This repository made that
mistake three times in one afternoon, in three different files, and each
instance was internally consistent arithmetic producing a confident wrong
answer — see [Phase 8](BUGS.md#phase-8-every-latency-figure-in-this-repository-was-wrong-by-100x).
The rule that now prevents it is mechanical rather than careful: a
reference-clock value is converted to nanoseconds **at the point of
capture**, and `Timer::now_ns()` is the only reference-clock accessor the
TSC code uses at all. There is no tick-valued timestamp left in that file
for a nanosecond constant to be added to.

**The measurement floor is measured, published and asserted, not
assumed.** `hft_tsc_bench` calibrates a TSC, reports the cost of every
clock primitive, sweeps a known workload to demonstrate the harness can
resolve a difference at all, and measures run-to-run drift over matched
windows. It exits non-zero if its own calibration window is the wrong
length. This is the difference between "decode is fast" and "decode is
below the floor on this host", and only one of those is a fact.

**Ticks are not cycles, and this repository will not pretend otherwise.**
On a modern x86-64 CPU the TSC is constant-rate and core-synchronised
while the core's actual clock moves with turbo and thermal limits, so a
tick delta cannot be converted into a cycle count without APERF/MPERF or
`perf_event_open`. Both clocks report nanoseconds here. Claiming cycles
from a tick delta is a 40% error waiting for a machine with turbo
enabled, and "cycles" is the more impressive-sounding unit of the two.

**Determinism is a feature.** Same capture, same book, same checksum, at
any optimisation level on any host. This is checked, not claimed:
`scripts/determinism.sh` and `scripts/determinism.ps1` build the replay
tool at `-O0`, `-O1`, `-O2`, `-O3`, `-Os` and `-Oz`, replay the same
generated capture in each, and fail if a single checksum disagrees. Both
bench scripts run it as a correctness gate, because a book checksum that
depended on `-O` would invalidate every latency figure in this file: the
thing being measured would not be the same program twice.

That test was added after this file had been claiming it for some time.
The `determinism` CTest runs `hft_replay` twice from a single binary,
which proves the engine repeats within a build and proves nothing about
the optimiser. A build with an uninitialised read that happened to zero
itself at every level tried would have passed it. The harness was
verified by injecting a level-dependent value and confirming it failed
before the claim was left standing. Without that property no benchmark
is reproducible and no bug is reproducible, and a latency number that
cannot be re-derived is an
anecdote.

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

**A MoldUDP64 Message Block is an ITCH frame, so the framing does not
translate anything.** A block is `[2-byte length][body]` and an ITCH
frame is `[2-byte length][body]`; `MessageBlocks::next()` returns the
block *including* its prefix so it can be handed straight to `decode()`
with no adjustment at either layer. The block length field excludes its
own two bytes, so a block occupies `length + 2` — a detail worth stating
because getting it backwards produces a packet that parses one block
short and then reads the next packet's header as message data.

Verified against the Nasdaq MoldUDP64 specification with a hand-built
packet in `tests/test_units.cpp`, including two facts this repository had
previously stated wrongly: the Sequence Number field is **eight** bytes
and MoldUDP64 has **no checksum**. See "What this is not".

**Routing an order is the hard part of sharding, not the books.** ITCH
carries a stock symbol in Add Order and in nothing else — Execute, Cancel
and Delete name only the order reference. So a multi-symbol handler
cannot decide which book a mutation belongs to by reading the message;
it has to remember. Three options, and the obvious one is rejected:

- *Search every shard* — O(shards) per mutation, and worse exactly where
  sharding was supposed to help.
- *Encode the shard in the reference number* — the venue lets the client
  choose it, so this is O(1) with no shared state, and many venues'
  documentation suggests it. Not used here: it puts a correctness
  requirement into a number that arrives from outside. If the reference
  does not carry the shard — a venue-assigned reference, a second client
  on the same feed — routing is silently wrong, and the symptom is a
  mutation applied to a book that never saw the order. That is not a
  crash. It is inventory created out of nothing.
- *Keep the index* — a table from reference to book, written by the Add
  that created it.

The third is what `include/hft/lob/shards.hpp` does. It needs no
cooperation from the venue and it degrades to "the order is unknown"
rather than "the order is on the wrong book". Because ITCH references
are unique for the trading day, the table is insert-only — no deletions,
no tombstones, and none of the clustering problems open addressing
acquires under long runs of removals.

**No locks, because the routing is arranged rather than synchronised.**
Sharding by symbol alone would put two symbols in one shard, and two
threads writing one index bucket. Instead the dispatcher owns *all*
routing state, so the sequence is: the dispatcher decodes, learns the
symbol on an Add, records the owner, and pushes to that worker's ring.
A mutation for that symbol cannot arrive before its Add, so the
dispatcher always knows where to send it — one hash and one probe, no
search and no shared mutable state. The workers never touch the index at
all. The trick is not making the index concurrent; it is arranging for
there to be exactly one thread that writes it.

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

**The price ladder is a dense array with an occupancy bitmap, and it was a
sorted linked list until measurement said otherwise.** Inserting a price
not adjacent to the best walked from the head of the ladder, so
book-update latency was proportional to ladder depth — 178x a touch
insert at 3,200 levels, linear in depth *and* in distance. It is now a
direct-indexed grid, `(price - floor) / tick`, with a per-side occupancy
bitmap: the same measurement reads 1.01x at 1,000 levels and 0.93x at
3,200, and the deepest row went from 28,324 to 155 ticks/op.

The bitmap is load-bearing, not an optimisation. The pure-array variant
gives O(1) adds but makes deletion of the last order at the touch O(M)
unless the best level is tracked incrementally — the documented failure of
the array approach. Best-bid is the highest set bit, best-ask the lowest,
so the touch is found without scanning order nodes.

It costs **2.5% on the average** and buys the entire tail. A real price
process inserts near the touch most of the time, so the average walk was
already short; eliminating a short walk saves little. What it guarantees is
that the 178x case cannot happen at all. That is a tail optimisation, and
whether you want one depends on whether your latency budget cares about
p999. It is opt-in via `LadderConfig`, with the hash map and linked list
retained as the out-of-band fallback, because a sparse instrument needs a
price range it does not have. Numbers in
[`results/OPTIMIZATION.md`](../results/OPTIMIZATION.md#the-dense-price-ladder-a-tail-fix-not-an-average-fix).

---

## Verifying a field table you cannot test against

**Order Replace and Broken Trade are implemented, and the reason they
were skipped for so long is worth keeping.** Both were declined on the
grounds that their field tables could not be verified. The specification
is public at `nasdaqtrader.com`; it was verifiable the whole time. What
made it *look* unverifiable is the real lesson: **the same message has
different offsets in ITCH 3.1 and 4.0**, both also published, so a table
copied from either is wrong in a way that looks right. Skip-by-length
was the right call under that uncertainty.

Going back to the source found two things:

- **The generator was writing a 41-byte Order Replace frame against a
  35-byte message** — the same four phantom fields (`order_type`,
  `time_in_force`, `display`, `participant`) that made the Add Order
  decoder wrong for this project's entire life, appended to a *different*
  message. The decoder had been skipping these frames by length, so no
  test ever read those bytes and the wrong size was invisible. Verified
  layouts now derive their length from the same constants the decoder
  reads, so the two cannot drift apart again.
- **`'B'` is not Order Entry.** It is the Broken Trade message, a 19-byte
  *inbound* report that an execution was cancelled under the
  clearly-erroneous policy — see the note under
  [What this is not](../README.md#what-this-is-not).

# History

The commit log in this repository is machine-generated: every entry is a
sequential version stamp with an empty body. That is an artefact of how
the work was produced, not a record of how it was reasoned about, and it
is close to useless to a reader who runs `git log` expecting to learn
something.

This file is the substitute. It is what the log would say if it had been
written at the time, and it exists so that the *order of discovery* is
legible — which is the part that matters, because in this project the
sequence is the argument.

The claim being made throughout is that the defects found here were
mostly not found by testing. They were found by comparing against
something outside the repository: a published specification, a second
compiler, an optimisation level, or a number that did not look right.
`docs/BUGS.md` has the catalogue grouped by that axis; this is the
timeline.

---

## The arc, in the order the things were learned

**1. A book, and a decoder that agreed with it.**
The limit order book came first: price-time priority, a slab arena with
integer handles instead of pointers, `int64_t` fixed-point prices at
1/10000 to match ITCH's native precision. Then the ITCH 5.0 codec over
it, zero-copy from one reusable buffer, big-endian fields reassembled
from a 48-bit timestamp split across two fields.

**2. Everything agreed, and all of it was wrong.**
This is the phase the rest of the project is built around. `Add Order`
read its share count as its price, because the decoder's field table had
the two fields swapped. The feed generator wrote the same wrong layout,
the differential test compared two models over *decoded* messages, and the
determinism harness proved a checksum was reproducible. **512 checks
passed.** The generator and the decoder were each other's reference, so
they agreed perfectly and were both wrong.

The same species of error appeared twice more — a 41-byte Order Replace
frame against a 35-byte message, and `'T'` listed where the specification
says `'P'` — and the non-cross-trade message was listed as `'T'`, a tag
no version of TotalView-ITCH has ever defined.

What fixed it was not a better test. It was reading the published field
table and asserting every offset against its *literal* value, so the two
cannot drift apart again, plus one test that assembles a frame byte by
byte from the table without going near the generator.

**3. The instrument was wrong before the engine was.**
`hft_bench` bracketed every stage with three `QueryPerformanceCounter`
reads per message. A QPC read costs about 27 ns on this host, so roughly
81 ns of every reported message was the clock reading the clock — 26% of
the measurement. The reported "344 ns per message" was about a quarter
clock. Removing the instrumentation moved identical work from 2.53M to
3.18M msg/s.

Then the worse one: **every latency figure in the repository was wrong by
a factor of 100.** `QueryPerformanceFrequency` returns 10 MHz on the
development host, so one tick is 100 ns, and tick deltas were being
recorded into histograms whose output was labelled nanoseconds. A p50
that printed `2` meant 200 ns. Throughput survived, because it is the
only figure computed through a conversion from an independent quantity —
total elapsed time. That is why it was the number that gave the others
away.

The rule that came out of it is mechanical rather than careful: convert
at the point of capture, and leave no tick-valued timestamp in the code
for a nanosecond constant to be added to.

**4. Optimising the wrong thing, four times.**
With the instrument fixed, three further changes were measured and
reverted. Interleaving a hash-map entry from three parallel vectors into
one 16-byte cache line — four per line, unambiguously better layout —
measured **−1.2%: nothing.** The reason is worth more than the change:
the three loads in a probe are *independent* addresses, so they issue in
parallel and expose roughly the latency of one. Memory-level parallelism
means three concurrent misses cost three times the bandwidth and about
the same latency. Collapsing a double probe found the second probe was
already an L1 hit. Lookahead prefetching failed because the two dominant
accesses **cannot be prefetched at all** — neither address is knowable
until the hash probes finish.

Measuring *where* the time went explained all four: decode is 12% of
per-message cost, `OrderBook::add` is the other 88%, and that 270 ns is
four dependent random accesses into 26–38 MiB structures. The constraint
is the **count** of memory round trips. Every failed attempt was in the
category of making existing accesses cheaper.

**5. The book could wedge, permanently, and no test noticed.**
`FlatMap::insert` reused a tombstone only when it also found an empty
slot in the same probe run. The book erases from its indexes on every
removal, so both tables fill with tombstones as it churns — and once every
slot is occupied-or-tombstone, **every future add returns
`capacity_exhausted` forever**, while the book looks nearly empty. A book
that traded a few hundred million shares would stop accepting orders.

It was present for the project's entire life and passed every test,
because every test sizes capacity far above its operation count, so
cumulative churn never reached capacity. It was found by writing a
benchmark that sized capacity *tightly*, which tripped it on the first
operation.

**6. The strategy was measuring a frozen market.**
After the decoder fix, the market maker's "volatility" turned out to be
share-count noise in a fake price. With real prices the book stopped
appearing to move: over 2,000,000 records at 1,000 levels a side the mid
moved **24 times**, so the decision stage was timing quoting arithmetic
against a frozen mid. The cause was two unrelated numbers — a walk drift
of four ticks against a ladder a thousand ticks wide.

It now moves **17,367 times in 2,000,000 records**, and the tool prints
that rate on every run, because a benchmark that cannot distinguish a
repricing book from a static one is measuring the wrong thing quietly.

Four fixes were tried here and three made it worse, which is the more
useful half: concentrating liquidity at the touch, removing the
furthest-from-mid removal policy (its documented justification —
crossing prevention — turned out to be false), and distance-biased
tournament selection. Capping at one order per level instead of three
worked.

**7. The ladder: measured, then removed.**
The price ladder was a sorted linked list, so inserting a level not
adjacent to the best walked from the head. `hft_ladder_bench` measured
it at **137.81x** a touch insert at 3,200 levels, linear in depth *and*
in distance — O(depth), confirmed rather than asserted.

Getting that measurement right took three attempts. The first sized
capacity at 4x the working set and measured `FlatMap` probe length
against a tombstone-saturated table (the wedge bug). The second put
levels on consecutive ticks, so every probe price **already existed**,
`find_level` hit, and no walk happened at all — an O(depth)
microbenchmark whose insertion points are occupied measures O(1) and
reports nothing. The third used a two-tick stride, leaving a gap at every
rank.

The fix is a direct-indexed price grid with a per-side occupancy bitmap,
implemented after reading how Kalshi and others build theirs. The bitmap
is load-bearing: the pure-array variant gives O(1) adds but makes
deletion of the last order at the touch O(M) unless the best level is
tracked incrementally. Worst case went from 137.81x to flat, 24,652 to
250 ticks/op. It costs 2.5% on the average and buys the whole tail.

It immediately produced two defects — a `bid_bits_` bitmap declared
`uint32_t` instead of `uint64_t`, and a best-word cache hint that
declared "empty" instead of rescanning — because it was the first thing
here implemented from the literature rather than derived. 450,000
differential operations caught both; review caught neither.

**8. Concurrency: a conclusion that reversed.**
The first concurrency tool reported that splitting decode from apply
**lost** 9–19%, and blamed load imbalance. That was a rationalisation for
a bug: the consumer drained the ring, found it empty, *then* checked the
producer's stop flag, and the producer could push more messages and set
the flag in between. The run did less work and timed as slower.

With the drain fixed the split is break-even, and the tool now *computes*
that verdict from its own best ratio against its own measured noise floor
rather than asserting it — because an earlier revision printed "BREAK
EVEN" as a hardcoded string while the table above it read 1.10x–1.13x. A
conclusion that cannot be refuted by the measurement printed beside it
is not a result.

Sharding by symbol is where threading pays: **1.85x** from a dispatcher
plus two workers, flattening past that because the dispatcher decodes and
routes every message and is therefore a serial floor. That floor, not the
worker count, is the number to quote for the design.

**9. Documentation that contradicted itself.**
The repository's own consistency checks were the last thing built, and
they exist because the prose had accreted corrections instead of
replacing them. Three sections came to contradict each other while every
link between them still resolved.

They found real drift immediately. This file's predecessor README claimed
**783 checks** while `hft_test` printed **1,412** — a published number
wrong by more than 2x, with every test green. The dense price ladder was
documented as both shipped and unshipped in four places at once, including
a benchmark whose own output said the fix "is NOT implemented here" while
measuring it. And `check-claims` now fails the build when any of this
recurs, which is the only reason to trust a number in prose.

---

## What the sequence actually argues

The three headline bugs were not found by testing, and none of them could
have been. They were found by comparing against something outside the
repository — a published field table, a second compiler, a run-to-run
difference, a ratio that did not look right.

The practical consequence is the shape of the project. Wire layouts are
asserted against the specification rather than against each other. The
book is differentially fuzzed against a naive model. The same capture
must produce one checksum at six optimisation levels. The clock's
resolution floor is measured, published and asserted before any latency
figure is read. And the numbers this README publishes are checked against
what the binaries print.

Each of those is a small, unglamorous piece of machinery, and together
they are the argument: **a system that verifies its own claims, and says
plainly which claims it has not yet earned.**
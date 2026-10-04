# Results: the measurement argument

The tables live in [README.md](../README.md#results). This document is
the reasoning behind them -- specifically the parts where a plausible
explanation turned out to be wrong, which is the only reason any of it is
worth reading.
**Not every figure in the README is published yet.** The `[MEASURED]`
placeholders in the latency tables are deliberate: the development host's
clock has 100 ns granularity, which makes the fastest pipeline stage
unresolvable rather than fast. Throughput, ratios and counts *are*
published, because a ratio cancels the clock overhead and a throughput is
computed from total elapsed time. See
[`results/ENVIRONMENT.md`](../results/ENVIRONMENT.md).

---

## Throughput: four fixes, three of which made it worse

**A benchmark of a book that does not reprice measures nothing, and for
a while that was what the deep shape was.** Over 2,000,000 records at
1,000 levels a side the mid moved 24 times — 0.001% of observations —
so the decision stage was timing quoting arithmetic against a frozen mid.
That is the arithmetic and not the behaviour, and the number was
misleading in the direction that flatters.

The cause was not the book, it was two numbers that had nothing to do
with each other. The walk had a fixed drift of four ticks while the
ladder was a thousand ticks wide, so resting orders spread over a band
two hundred times wider than the price ever travelled and the best bid
became the maximum over that band — a stable extreme-value statistic
rather than a price anyone was quoting. Measured at 1,000 levels over
150,000 records: reversion 64 gave 5 mid moves, 500 gave 50, 1000 gave
220. Drift is now scaled to the ladder and the anchor with it.

The deep shape now moves the mid **17,367 times in 2,000,000 records,
0.87% of observations**, up from 24, with the book uncrossed throughout
and no truncation. The tool prints the measured rate on every run and
distinguishes a repricing book from a static one rather than bucketing
both under a threshold.

Four fixes were tried along the way and three of them made it worse,
which is the more useful half of the result:

- **Concentrating liquidity toward the touch**, on the theory that real
  books are front-loaded. Worse everywhere: 3 to 27 mid moves against
  uniform's 54 to 61. A thick touch is a sticky touch.
- **Removing the furthest-from-mid removal policy**, on the theory it
  was pinning the touch. It was not the cause, and its documented
  justification — crossing prevention — is false: with adds clamped
  against the resting book, a thousand-level book across several
  hundred thousand records was never crossed once either way.
- **Distance-biased tournament selection** as a model of stale-quote
  cancellation. Also worse: 4 to 14 moves. The measured fact is that a
  recent-biased draw does *not* remove stale orders, which is why the
  all-or-nothing policy was outperforming it.
- **Capping at one order per level** instead of three. This one worked.
  Three orders of slack at the touch is three orders too many; the touch
  never empties.

The shallow shape went from 23 mid moves to 130,270 over the same
rewrite, so the shallow-versus-deep ratio — the most load-bearing
measurement in this repository — is now a comparison of depth rather
than of two different price processes.

## Concurrency: the conclusion that reversed

The difference between those two tables is the whole argument for symbol
sharding. Splitting decode from apply leaves the expensive half — the
ladder walk and the slab edit — running serially on one core, and adds a
hand-off to pay for. Sharding by symbol gives every thread its own book
to apply into, so the expensive work is parallel *and* balanced. Every
sharded row is checked to have produced books byte-identical to the
single-threaded baseline; a pipeline that misroutes a mutation looks
exactly like a pipeline that is fast.

Ratios flatten as workers are added because the dispatcher decodes and
routes **every** message and is therefore a serial floor. That floor is
the number to quote for a design like this, not the worker count.

**An earlier version of this tool reported the opposite conclusion, and
the correction is the more useful half.** It measured the split at
0.91x“0.81x and attributed the loss to load imbalance. The
load-imbalance story was a rationalisation: the consumer drained the
ring, found it empty, and then checked the producer's stop flag, and the
producer could push more messages and set that flag in between. The
consumer exited with messages still queued, so the run did less work and
timed as slower. With the drain fixed, the split is break-even.

Two lessons worth more than the numbers. A benchmark reporting a
*slowdown* deserves the same suspicion as one reporting a speedup — and
the checksum column is what caught this, because books that differ from
the baseline are reported as INVALID rather than as a result. And a
plausible mechanism is not a verified one.

---

## Price ladder: the O(depth) walk, measured

The price ladder is a sorted doubly-linked list, so inserting a level that
is not adjacent to the best walks from the head of the ladder. `hft_ladder_bench`
measures that walk, and the measurement is cleaner than expected.

```
hft_ladder_bench
```

**Ratios, not nanoseconds.** The development host's clock has 100 ns
granularity, so an absolute add/remove latency here is a floor reading. A
*ratio* survives that, because the clock-pair cost is a roughly constant
additive term which inflates numerator and denominator alike and cancels in
the quotient. Every figure below is therefore a **lower** bound: a ladder
that looks 178x worse is at least 178x worse, and cannot look worse than it
is.

**The control is the point.** `distance = 1` walks a single node, so it
measures all of add/remove except the ladder. It sits at 160-230 ticks
across every depth from 10 to 3200 — flat, as it must be, since a one-node
walk costs the same whatever the ladder size. Any signal in the other
columns is the ladder.

| depth | distance 1 | distance/4 | distance/2 | distance = depth |
|---|---|---|---|---|
| 10 | 1.0x | 1.4x | 1.4x | 1.3x |
| 100 | 1.0x | 3.0x | 3.6x | 5.7x |
| 320 | 1.0x | 5.7x | 9.7x | 17.0x |
| 1000 | 1.0x | 10.1x | 20.5x | 39.7x |
| 3200 | 1.0x | 47.3x | 89.9x | 177.6x |

Linear in distance at fixed depth (at depth 1000: 10.1x, 20.5x, 39.7x for
quarters, halves, all), and linear in depth at fixed distance (full-depth
walk: 5.7x, 17.0x, 39.7x, 177.6x for depths 100, 320, 1000, 3200). That is
O(depth), confirmed rather than asserted.

**So what?** At the depths a liquid US equity actually shows — tens of
levels — the walk is a rounding error against the ~55 ns control. At 3200
levels it is 178x the control, and it would dominate the book update
entirely. That is a real cost, but on this workload it is not the cost:
end-to-end barely moves when the walk is eliminated (see below), because
the average case was already dominated by the order-reference hash and the
pool accesses.

## The depth-proportional walk, measured and removed

The fix is a **direct-indexed price ladder**: an array indexed by price
offset from an anchor, with a per-side occupancy bitmap, making both "find
the level at this price" and "insert at the head" O(1). It is implemented,
opt-in via `LadderConfig`, with the hash map and linked list retained as
the out-of-band fallback so a sparse instrument keeps working.

| depth | walk = depth, sparse | walk = depth, dense |
|---|---|---|
| 1,000 | 39.69x | **1.01x** |
| 3,200 | 177.63x | **0.93x** |

Flat across every distance at every depth: the walk is gone. In absolute
terms the deepest row went from 28,324 ticks/op to 155 — **183x**.

**And it costs 2.5% on the average.** End-to-end on a realistic bounded
book: sparse 2,983,961 msg/s, dense 2,908,714 msg/s. Both numbers are
true, and the gap between them is the whole point. A real price process
inserts near the touch most of the time, so the *sparse* book's average
walk is short and eliminating a short walk saves little. The dense
ladder's value is that the 177x case cannot happen at all. **This is a
tail optimisation**, and whether you want it depends on whether your
latency budget cares about p999. In a market maker it does.

The occupancy bitmap is not optional, because the pure-array variant
trades the add for a different pathology: "will give O(1) always for add
operations, but at the cost of making deletion/execution of the last order
at the inside limit O(M)" unless the best level is tracked incrementally.
Best-bid is the highest set bit, best-ask the lowest.

Full account, including the two bugs the change introduced and the
documented research it was built from, in
[`results/OPTIMIZATION.md`](../results/OPTIMIZATION.md#the-dense-price-ladder-a-tail-fix-not-an-average-fix).

### Getting the experiment right took three attempts

The measurement above is the third version, and the two failures are more
instructive than the result:

1. **First attempt: capacity sized at 4x the working set.** Produced ~900 ns
   per add/remove, scaling with depth and flat in walk distance. That is
   not a ladder walk — it was FlatMap probe length against a
   tombstone-saturated table (see the wedge bug above).
2. **Second attempt: capacity sized generously, levels on consecutive
   ticks.** Still flat. The `distance` probes were landing on prices that
   **already existed**, so `find_level` hit and no walk happened at all —
   the tool was measuring an add to an existing level while labelling it a
   walk.
3. **Third attempt: levels on a two-tick stride, leaving a gap at every
   rank.** Now any walk length from 0 to depth is reachable, the probe
   asserts that a level was actually created and destroyed, and the signal
   is clean.

The generalisable lesson is the second one: **an O(depth) microbenchmark
whose insertion points are already occupied measures O(1) and reports
nothing.** The tool now verifies the level count changes across the probe,
so it cannot silently measure the wrong operation again — and it refuses to
print a row at all if the probe does not create a level.

# The hardware path

> A second implementation of the market data path, in VHDL, differentially
> verified against the C++ engine in the parent directory.

---

## What this is for

Two things, and only two.

**1. A second source of truth.** The worst bug in this repository's
history was not subtle: the Add Order decoder read the share count as
the price. It survived 512 green checks, a 400,000-operation differential
test, and a determinism harness, because every one of those compared the
engine against **itself** — the generator wrote the same wrong layout
the decoder read, so the two agreed perfectly and both disagreed with
the specification.

An independent implementation of the same protocol, written from the
published field tables and compared field by field, is a check that
cannot share an assumption with the thing under test. When the two
disagree, one of them is wrong, and the specification says which.

**2. Cycle-exact measurement.** The engine's latency table is
`[MEASURED]`-blank on the development host, because that host's clock
ticks every 100 ns and the fastest pipeline stage is below the floor. A
rented bare-metal host fixes it, at a cost, for a number that is still
only as good as the host.

In RTL, a cycle count is a property of the design, not of the machine
running it. It is exact, it reproduces on any simulator, and it is
checkable in CI. The two engines then measure the same pipeline two
ways, and the comparison between them is a real result rather than an
artefact of a clock.

Neither of these is a claim that hardware beats software here. On a
consumer x86 the software is faster, and the reason is interesting
rather than flattering: the software's cost is four *dependent DRAM round
trips* per order update, and the RTL working set stays on chip. That
difference is a property of the memory hierarchy, not of the
implementation, and it is worth being able to state precisely.

## The contract

[`TRACE_FORMAT.md`](TRACE_FORMAT.md) is the specification: the capture
format, the trace record format, the field lists, and the rules for
when a decoder records a message type and when it does not.

It is the whole interface between the two implementations. The C++ side
implements it in `src/rtl/trace.hpp` and `src/rtl/decode_trace.hpp`; the
RTL side implements it in VHDL, from the document, and shares no code
with the C++ side. A shared header would defeat the purpose: a mistake
in one implementation would hide inside the other.

## The tools

Both are built by the parent directory's CMake and run from the build
directory.

```bash
# A capture, read frame by frame, written as a trace.
# With no capture path, one is generated in memory, deterministically.
./build/hft_rtl_trace --out /tmp/a.trace --hex /tmp/a.hex --records 2000

# A specific capture, and a specific generator seed.
./build/hft_rtl_trace real.itch --out /tmp/a.trace --seed 0xC0FFEE0012345678

# A capture cut short, to exercise the truncated path.
./build/hft_rtl_trace --out /tmp/t.trace --truncate-at 4096

# Two traces, compared. Exits non-zero if they differ.
./build/hft_rtl_check /tmp/a.trace /tmp/rtl.trace
```

The comparator is deliberately ignorant of which side is which. It
compares two files and reports the first difference, and nothing else.

## Current state

| Piece | State |
|---|---|
| The trace format, specified in prose | done — [`TRACE_FORMAT.md`](TRACE_FORMAT.md) |
| The C++ implementation of it | done — `src/rtl/trace.hpp`, `src/rtl/decode_trace.hpp` |
| The reference capture walk | done — `src/rtl/capture_walk.hpp` |
| The trace generator and comparator | done — `src/rtl/rtl_trace.cpp`, `src/rtl/rtl_check.cpp` |
| The format, the comparator and the bridge, tested | done — 85 checks, `tests/test_rtl_trace.cpp` |
| The tools end to end, including that the comparator can fail | done — `cmake/rtl_trace_check.cmake` |
| **The RTL model** | **not started** |
| The RTL decode path in VHDL | not started |
| The RTL capture walk in VHDL | not started |
| Cycle-exact pipeline measurement | not started |
| The order book in RTL | not started |

The last four are the next phase. The first five exist so that the RTL
has something to be measured against, and so that the harness is known
to work before there is anything for it to disagree with.

The order book in RTL is deliberately last. It is the most interesting
piece and the most expensive, and it is worthless without the decode
path it would be built on.

## Why the reference walk is not the engine's reader

The engine has a capture reader already, and the reference walk in
`src/rtl/capture_walk.hpp` deliberately does not use it.

The differential is meant to compare two *independent* implementations
of the market data path. If the C++ side of the comparison reused the
engine's reader, the framing would be the engine's framing on both
sides of it, and a framing mistake would be invisible — both sides
would be reading frames the same wrong way.

Writing the walk out separately means the framing is part of what is
being compared. It is also a specification in prose: it is the readable
answer to "where exactly does a message end?", for anyone implementing
the other side.

The reference walk is checked against the engine's reader by
`tests/test_rtl_trace.cpp`, which asserts that the two agree on every
frame boundary of a generated capture. The chain of trust is: RTL walk
against the reference walk by the differential, reference walk against
the engine's reader by the test suite, and the engine's decoder against
the specification by the hand-built frames.

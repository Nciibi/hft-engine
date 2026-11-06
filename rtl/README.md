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
| **The RTL field tables** | done — `hft_itch_pkg.vhd`, a second transcription of the spec |
| **The RTL capture walk** | done — `hft_moldudp64.vhd` |
| **The RTL decoder** | done — `hft_itch_decode.vhd`, single-pass streaming |
| **The RTL trace writer** | done — `hft_trace_fmt.vhd`, `tb/tb_trace.vhd` |
| **RTL against C++, 2,000 messages, field by field** | done — 0 differences |
| **RTL against C++, over a capture cut mid-message** | done — 0 differences |
| **Cycle-exact decode cost** | done — one cycle per body byte, measured |
| **The RTL order book** | done — `hft_book.vhd`: order pool, sparse level list, dense grid, occupancy bitmap, sparse fallback |
| **The synthesizable RTL book** | done - `hft_book_hw.vhd`: inferred block RAMs, 2.0 Mbit, 85F; read result path pipelined |
| **Hardware book against the C++ book** | done - 0 differences on generated and real captures |
| **The RTL book against the C++ book** | done — 0 differences in sparse, dense and fallback runs |
| **Cycle-exact book cost** | done — 41.75 cycles/message sparse, 5.28 dense, 25.96 fallback |
| **Both differentials over REAL Nasdaq messages** | done — decoder 70,208 messages, book 1,792 sparse and dense; 0 differences |
| Synthesis, area and timing | done — local YoWASP reproduction: framer 183.59 MHz, decoder 123.35 MHz, hardware book 82.13 MHz (100 MHz miss, published); [`../results/SYNTHESIS.md`](../results/SYNTHESIS.md) |

## The result

```
$ ctest --test-dir build -R rtl -V
  rtl simulation .... rtl trace records 2000
  rtl decode cycles63444
  differential ...... the two implementations agree on every record and field

  rtl book .......... rtl messages     1000
  rtl book cycles  41750
  rtl cycles/msg   41
  rtl dump records 974
  rtl checksum     97a2bb9075066e99
  differential ...... the two books agree on every order, level and field
```

**Two numbers the C++ side cannot produce.**

**63,444 cycles to decode 2,000 messages** — a mean of 31.7 cycles per
message, because the RTL decoder takes exactly one clock cycle per byte
of message body, and the mean is the mean body length of the feed.

**41,750 cycles to apply 1,000 messages to the sparse book** — a mean of
41.75 cycles per message. That number is dominated by the price walk, and
it scales with the depth of the book exactly as an O(depth) walk must.
The dense grid removes the walk; it is flat at about 5.25 cycles per
message across both depths:

| Records | Levels per side | Sparse | Dense | Narrow-band fallback |
|---|---|---|---|---|
| 1,000 | ~240 | **41.75** | **5.28** | **25.96** |
| 2,000 | ~440 | **71.40** | **5.25** | **40.52** |

The fallback figures use the same deliberately narrow band: in-band
prices use the grid, the rest use the list, and the merged dump must
still be in priority order. At 1,000 records the band costs 25.96 cycles
per message; at 2,000 the book is deeper and more of it lies outside the
band, so the cost rises to 40.52. That is what a bounded band should do,
and it is why the differential runs all three cases rather than only the
one that flatters the structure.

These are properties of the *design*, not of the machine: they reproduce
on any simulator, on any host. The 1,000-record cases are checked in CI;
the 2,000-record figures come from the same script with `RECORDS=2000`.
The C++ can report the same phenomenon only as a ratio —
`results/OPTIMIZATION.md` measures the sparse ladder's walk at up to
178x a touch insert at 3,200 levels — and a ratio is a statement about a
machine rather than about a design. Here the same number is a cycle
count.

## The same differential, over real market data

```
$ ctest --test-dir build -R rtl_real -V
  real data ........ fetching 4194304 compressed bytes from the Nasdaq ITCH archive
  field tables ..... every decoded type has the length the table says
  rtl decode ....... rtl trace records 70208
  rtl decode cycles1934929
  differential ...... the two decoders agree on every field of every real message
  rtl book .......... rtl messages     1792
  rtl book cycles  279035
  differential ...... the two books agree on every order, level and field
  rtl hardware ...... rtl messages     1792
  rtl book cycles  557588
  hardware differential ...... the hardware and C++ books agree
  rtl dense ......... rtl messages     1792
  rtl dense cycles  10284
  differential ...... the sparse and dense books agree on every order, level and field
```

**70,208 real messages from the exchange**, and the RTL decoder and the
C++ engine agree on every field of every one of them. The book
differential uses a separate 64 KiB slice - 1,792 messages - because
the sparse walk costs more per message as the real book deepens; the two
books still agree on every order, level and field.

The same slice also runs through the dense grid. Real prices span raw
values 800 to 100,000,000, so that run uses a 100-unit tick and a
1,000,001-slot band. Prices that do not fall on the tick, and prices
outside the band, take the sparse fallback, so the merged dump is
exercised on real messages too. It matches the sparse book and takes
10,284 cycles, 5.74 per message.

This is the test that found two defects that generated data could not:
the Order Executed field table had a byte the message does not have, and
the RTL index probe used a slot it had not examined. Both are written up
in [`../docs/BUGS.md`](../docs/BUGS.md).

The data is fetched, not committed: the exchange's files are several
gigabytes and are the exchange's property.
[`../scripts/fetch-itch.sh`](../scripts/fetch-itch.sh) fetches a slice
with an HTTP Range request, and the test skips itself when the network
is not there.

## The order book

The RTL book models the C++ book exactly: an order pool, a level pool,
a FIFO of orders within each level, and an open-addressed index from the
ITCH order reference to the order. Levels live either in a price-ordered
sparse list or in a dense grid: a slot is
`(price - PRICE_FLOOR) / PRICE_TICK`, the per-side occupancy bitmap
makes the best level a bit scan, and prices outside the configured band
fall back to the list. `DENSE` is off by default, so the original
behaviour remains the regression path.

**The index never erases.** ITCH order references are unique for the
trading day, so a reference whose order has left the book will never
be looked up successfully again, and the slot it occupied can keep its
key and be marked not live. The C++ does erase, and pays for it: its
table accumulated tombstones until a defect made it wedge permanently
once every slot was occupied or tombstoned, and a book that traded a
few hundred million shares would have stopped accepting orders. An
insert-only table cannot wedge in that way, because it never needs to
reclaim a slot.

**The RTL grid takes `PRICE_FLOOR` and `PRICE_TICK` in raw price units.
A price below the floor, outside the band, or not on a tick boundary is
not addressable in the grid and falls back to the sparse list. The
generated differential uses tick one; the real-data book also runs with
a 100-unit tick and a 1,000,001-slot band, because its observed prices
span 800 to 100,000,000 raw units. `PRICE_FLOOR` is a natural, so
negative raw prices remain outside the model.**

**The hardware core is a separate implementation of the sparse ladder.**
`hft_book_hw.vhd` keeps the same interface and dump format, but stores
the order pool, level pool and reference index in synchronous arrays so
GHDL infers block RAM. It sequences one access per cycle, and it is
checked against the C++ dump on both the generated capture and the real
slice. With 4,096 orders, 2,048 levels and 8,192 index slots the three
memories are about 2.0 Mbit, which is why synthesis places the core on
an 85F rather than the 25F. The hardware run costs 162.86 cycles per
message on 1,000 generated messages and 617.35 on the 1,792-message real
slice. The read result path is pipelined one stage beyond the first
fabric register; those are not improvements on the behavioural model, and
they are the price of real memories and registered reads. The behavioural
model remains the cycle model.

**The comparison is a dump, not a checksum.** A checksum says "these two
states are equal"; a dump says "these two states are equal, and if they
are not, here is the order and the field that differ". The difference
matters when the answer is no — and this repository has shipped a wrong
answer before, in one field of one message, that a checksum would have
reported as "these differ" and nothing else.

The dump carries the level records as well as the order records, and
that matters too: a book can have the right orders in the right order
and still have an inflated level aggregate, and the checksum does not
cover the aggregate at all.

## Running it

```bash
# The C++ side: a capture, its hex sidecar, and a trace of it.
./build/hft_rtl_trace --out /tmp/cpp.trace --hex /tmp/capture.hex --records 2000

# The RTL side, over the same capture.
ghdl -a --std=08 --workdir=/tmp/ghdl rtl/hft_itch_pkg.vhd rtl/hft_trace_fmt.vhd \
    rtl/hft_moldudp64.vhd rtl/hft_itch_decode.vhd rtl/tb/tb_trace.vhd
ghdl -r --std=08 --workdir=/tmp/ghdl tb_trace \
    -gHEX_PATH=/tmp/capture.hex -gOUT_PATH=/tmp/rtl.trace

# The comparison.
./build/hft_rtl_check /tmp/cpp.trace /tmp/rtl.trace
```

`ctest` runs all of it:

```bash
ctest --test-dir build -R rtl --output-on-failure
```

GHDL is not a dependency. Where it is absent the differential is
**skipped** rather than failed, because a missing simulator is a
missing simulator and not a wrong answer — the same rule the doc check
follows. The CI job that installs it turns a skip into a failure.

### Under chronam

[`chronam.toml`](chronam.toml) makes this directory a project for
[chronam](https://github.com/Nciibi/chronam), the VHDL toolkit in the
same workspace:

```bash
./build/hft_rtl_trace --out cpp.trace --hex capture.hex --records 2000
chronam simulate
```

The waveform shows the framing state machine and the field parser
moving through a real capture, which is the fastest way to understand
what the decoder is doing. The trace it writes is the same format the
C++ side writes, so `hft_rtl_check cpp.trace trace.txt` compares the
two.

## The parts, and what each one is for

| File | What it is |
|---|---|
| `hft_itch_pkg.vhd` | The field tables: offsets, widths, body lengths, and which types are keyed on an order reference. A **second transcription** of the published specification, independent of the C++ one. |
| `hft_moldudp64.vhd` | The framing: Downstream Packet Header, Message Count, Message Blocks. The RTL side's own capture walk. |
| `hft_itch_decode.vhd` | The decoder. Single-pass streaming: it extracts each field as that field's bytes arrive, and never holds the message anywhere but in the pipeline. |
| `hft_book_pkg.vhd` | The book's types: the order states, the outcomes, and the FNV-1a constants. |
| `hft_book.vhd` | The order book: order pool, level pool, price-ordered level list, per-level queue, order-reference index, and the dump walk. |
| `hft_book_hw.vhd` | The synthesizable book: synchronous order, level and index memories, a read-wait state machine, and the same dump walk. Inferred RAMs; 85F target. |
| `hft_hex_reader.vhd` | The hex sidecar reader: a file of hex, as a byte stream, one byte per clock with no gaps. |
| `hft_trace_fmt.vhd` | Rendering a decoded message as a trace record: the status character, the decimal, the hex. |
| `tb/tb_trace.vhd` | The decode testbench: reads the hex sidecar, runs the design, writes the trace. |
| `tb/tb_book.vhd` | The book testbench: decodes the capture, applies every message to the book, writes the book dump. |

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

## VHDL-2008, and what that costs

The RTL is written against VHDL-2008 and nothing later, because the
simulator available in CI is GHDL 4.x and the two have differed on what
the standard means. Two things that cost a rewrite and are worth
recording:

- **A concurrent `report` statement is legal in VHDL-2019 and not in
  VHDL-2008.** Every report in this directory is therefore inside a
  process. It is a small thing, and it is the kind of small thing that
  passes on the author's machine and fails in CI.
- **A function's return type must be an unconstrained type mark.**
  `return std_logic_vector(7 downto 0)` is a parse error; a function
  returns an unconstrained value, and `return std_logic_vector` is the
  correct spelling.

And one that is a tool quirk rather than the standard: **a context
clause applies to the design unit that follows it and to that unit's
associated architecture, not to the next unrelated entity.** Which is
why every file here has one primary design unit.


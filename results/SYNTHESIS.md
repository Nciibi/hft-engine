# Synthesis: area and timing on ECP5

> **Revision after the read-path pipeline.** The figures below were
> reproduced locally with GHDL 6.0.0, YoWASP yosys 0.70 and YoWASP
> nextpnr-ecp5 0.11.1. They are not the native OSS CAD Suite build; the
> native run may differ, and CI is the publication gate.

The RTL is simulated, not placed on a board. This report is the
smallest useful hardware check that does not require one: synthesise the
design, place and route it out of context, and report what the tools
say. No pin plan, no bitstream, no board.

The framer and decoder use an **LFE5U-25F, CABGA381**. The hardware book
uses an **LFE5U-85F, CABGA381**, because its state is about 2 Mbit and
the 25F has only 56 DP16KDs. Both are constrained at **100 MHz**.
Out-of-context timing is a property of the logic, not of a package
escape.

## Result

Equality constraint: 100 MHz, CABGA381. The framer and decoder use an
**LFE5U-25F**; the hardware book uses an **LFE5U-85F** because its state
is about 2 Mbit and the 25F has only 56 DP16KDs.

| Design | Device | LUT4 | FF | BRAM | Target | Fmax | Outcome |
|---|---|---:|---:|---:|---:|---:|---|
| `hft_moldudp64` | LFE5U-25F | 172 | 77 | 0 | 100 MHz | **183.59 MHz** | met |
| `hft_itch_decode` | LFE5U-25F | 500 | 2,243 | 0 | 100 MHz | **123.35 MHz** | met |
| `hft_book_hw` | LFE5U-85F | 4,325 | 3,631 | **110** | 100 MHz | **82.13 MHz** | not met |
| `hft_book` | LFE5U-25F | - | - | - | 100 MHz | - | behavioural; not synthesised |

The framer has ample margin. The decoder meets the constraint in this
local reproduction; the earlier native report had a different decoder
number, which is a toolchain difference rather than a change in the
decoder source for this run. The hardware book does **not** meet
100 MHz. The timing report is the design's, not a tool artefact: the
achieved post-route figure is 82.13 MHz with this flow. The report says
so rather than hiding the row or changing the constraint.

## The hardware book

`hft_book_hw.vhd` is the synthesizable counterpart of the behavioural
book. It keeps the same external interface and the same dump format.
GHDL infers three block RAMs:

| Memory | Width | Depth | Bits |
|---|---:|---:|---:|
| `orders` | 235 | 4,096 | 962,560 |
| `levels` | 193 | 2,048 | 395,264 |
| `id_tab` | 79 | 8,192 | 647,168 |

That is 2,004,992 bits, or about **2.0 Mbit**. Yosys maps it to
**110 DP16KD** blocks; the 85F has 208, so the memory uses 52% of the
device. The book also uses 4,325 LUT4s and 3,631 FFs. It does **not**
fit the 25F's 56 DP16KDs, which is why it is on the 85F.

The hardware core is verified, not merely synthesized. It is run against
the same C++ dump as the behavioural model:

- generated capture: 1,000 messages, **162,859 cycles**, 974 dump records,
  checksum `97a2bb9075066e99`;
- real Nasdaq slice: 1,792 messages, **1,106,288 cycles**, 2,765 dump
  records, checksum `95aa35f0045c196e`;
- both dumps are field-for-field identical to the C++ book.

The hardware book is slower than the behavioural model because it
sequences one synchronous memory access per cycle and now pays for a
registered read pipeline: `st_read_wait`, `st_read_data`, `st_read_pp`.
That is the cost of making the storage real; the cycle numbers above are
not a speed claim against the behavioural model. The sparse behavioural
book costs 41.75 cycles per message on the generated capture; the
hardware core costs 162.86. On real messages the hardware core costs
617.35 cycles per message.

Timing is a miss. In this local reproduction the 85F run closes at
**82.13 MHz**, not 100 MHz. The read result path is pipelined; the
critical path remains the block-RAM output into the first fabric
register. The next step is not "add another fabric register" — that was
tried — it is either inference of the DP16KD output register through a
flow that supports it, or board-level placement constraints after the
core is integrated. The dump path is not separately pipelined because
the current critical path does not start there. The report does not
round the number up.

## The behavioural book

`hft_book` remains deliberately outside the synthesizable set. Its order
and level pools are process variables, so no RAM is inferred, and the
dense grid's bitmap scans use loop bounds that GHDL requires to be
static. GHDL therefore rejects it during synthesis. (Before the dense
grid was added, the sparse-only model expanded its pools into roughly
122 MB of Verilog and Yosys could not hold it; either way the memory
never becomes a RAM.) The generated report always carries the row so
that "this part is not hardware" stays visible.

## How it is produced

The flow is:

1. `ghdl --synth --std=08` turns a top-level VHDL entity into Verilog.
2. `yosys -p "read_verilog -sv ...; synth_ecp5 -top ... -json ..."`
   maps it to ECP5 primitives.
3. `nextpnr-ecp5 --25k` or `--85k --package CABGA381 --freq 100
   --out-of-context` places and routes it; `--write` emits the post-route
   JSON and `--report` emits the utilisation report.
4. The report parser takes the **last** `Max frequency for clock 'clk'`
   line, which is the post-route figure; the first is a placement
   estimate. Area is read from nextpnr's device utilisation, not from
   Yosys cell counts.

`--timing-allow-fail` is intentional. A timing miss is a measurement and
must be reported; only a tool failure should stop the run. `--write` is
used instead of `--textcfg` because an out-of-context run has no
bitstream to write, and asking nextpnr for one fails before the timing
report is produced.

The device is selected per top: `hft_book_hw` is placed on the 85F
because its inferred memories are 110 DP16KDs; the framer and decoder
stay on the 25F.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build -R rtl_synthesis -V
```

The generated table is written to `build/rtl_synth_work/synthesis.md`.
The test is skipped when yosys or nextpnr is absent, unless configure was
given `-DHFT_REQUIRE_SYNTH=ON`.

To reproduce the behavioural book failure:

```bash
cmake -S . -B build -DHFT_SYNTH_BOOK=ON
cmake --build build --parallel
ctest --test-dir build -R rtl_synthesis -V
```

The generated behavioural row is expected to record the synthesis
failure, not a misleading area number. The test reports the row and does
not pretend the behavioural design was synthesised.

## Tools

The figures above were produced with:

- GHDL 6.0.0
- Yosys 0.70+157
- nextpnr-ecp5 0.11.1-65

The exact version strings are recorded in the generated report, because
a synthesis number without its toolchain is an anecdote.

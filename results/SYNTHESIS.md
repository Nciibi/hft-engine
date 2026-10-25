# Synthesis: area and timing on ECP5

The RTL model is simulated, not placed on a board. This report is the
smallest useful hardware check that does not require one: synthesise the
design, place and route it out of context, and report what the tools
say. No pin plan, no bitstream, no board.

Target: **Lattice ECP5 LFE5U-25F, CABGA381**, constrained at **100 MHz**.
Out-of-context timing is a property of the logic, not of a package
escape.

## Result

| Design | LUT4 | FF | BRAM | Target | Fmax | Outcome |
|---|---:|---:|---:|---:|---:|---|
| `hft_moldudp64` | 169 | 77 | 0 | 100 MHz | **182.32 MHz** | met |
| `hft_itch_decode` | 1,111 | 2,230 | 0 | 100 MHz | **52.72 MHz** | not met |
| `hft_book` | - | - | - | 100 MHz | - | not synthesised |

The framer has ample margin. The decoder does not meet 100 MHz. It is a
single-pass streaming parser that takes one clock per body byte, and the
timing report is the design's, not a tool artefact: the achieved
post-route figure is 52.72 MHz. The report says so rather than hiding
the row or changing the constraint.

The book is deliberately outside the synthesis set. Its order and level
pools are process variables, so no RAM is inferred, and the dense grid's
bitmap scans use loop bounds that GHDL requires to be static. GHDL
therefore rejects the book during synthesis. (Before the dense grid was
added, the sparse-only model expanded its pools into roughly 122 MB of
Verilog and Yosys could not hold it; either way the memory never becomes
a RAM.) The generated report always carries the book row so that "this
part is not hardware yet" stays visible.

For context, the full project state is about 1.9 Mbit. The 25F has 56
DP16KD BRAMs; an 85F has 208. A hardware book would need a different
memory organisation and a larger device, which is future work rather
than a result being claimed here. See
[`../rtl/README.md`](../rtl/README.md) for the model and the dense-grid
differential.

## How it is produced

The flow is:

1. `ghdl --synth --std=08` turns a top-level VHDL entity into Verilog.
2. `yosys -p "read_verilog -sv ...; synth_ecp5 -top ... -json ..."`
   maps it to ECP5 primitives.
3. `nextpnr-ecp5 --25k --package CABGA381 --freq 100 --out-of-context`
   places and routes it; `--write` emits the post-route JSON and
   `--report` emits the utilisation report.
4. The report parser takes the **last** `Max frequency for clock 'clk'`
   line, which is the post-route figure; the first is a placement
   estimate. Area is read from nextpnr's device utilisation, not from
   Yosys cell counts.

`--timing-allow-fail` is intentional. A timing miss is a measurement and
must be reported; only a tool failure should stop the run. `--write` is
used instead of `--textcfg` because an out-of-context run has no
bitstream to write, and asking nextpnr for one fails before the timing
report is produced.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build -R rtl_synthesis -V
```

The generated table is written to `build/rtl_synth_work/synthesis.md`.
The test is skipped when yosys or nextpnr is absent, unless configure was
given `-DHFT_REQUIRE_SYNTH=ON`.

To reproduce the book failure:

```bash
cmake -S . -B build -DHFT_SYNTH_BOOK=ON
cmake --build build --parallel
ctest --test-dir build -R rtl_synthesis -V
```

The generated book row is expected to record the synthesis failure, not a
misleading area number. The test reports the row and does not pretend the
design was synthesised.

## Tools

The figures above were produced with:

- GHDL 6.0.0
- Yosys 0.70+157
- nextpnr-ecp5 0.11.1-65

The exact version strings are recorded in the generated report, because
a synthesis number without its toolchain is an anecdote.

# The decode trace format

> The contract between the C++ engine in this repository and the RTL
> model that implements the same market data path a second time.
>
> **This document is the specification.** The C++ side implements it in
> `src/rtl/trace.hpp` and `src/rtl/decode_trace.hpp`. The RTL side
> implements it in VHDL, from this document, and nothing else. Neither
> side shares code with the other, because a shared header would let a
> mistake in one hide inside the other — which is the entire failure
> mode this differential exists to catch.

---

## 1. Why there is a trace at all

The strongest claim this repository can make about its decoder is not
"it is fast" and not "it passes its tests". It is that a second,
independently written implementation of the same protocol produces the
same answers.

That claim needs an artefact to be made against. The artefact is the
**trace**: a record, for every frame in a capture, of what the decoder
decided about that frame and the value of every field it extracted.

The obvious alternative — comparing the two implementations' raw output
bytes — does not work, because both of them are reading the same bytes.
A trace that echoed the frame body would agree no matter how wrong
either side was, and the bug this repository shipped for most of its
life (an Add Order decoder that read the share count as the price) would
have passed it without comment. What the trace records is the *extracted
values*, and two implementations that disagree about what the bytes mean
produce different traces.

---

## 2. The capture

A capture is a stream of **MoldUDP64 Downstream Packets**, from the
Nasdaq MoldUDP64 Protocol Specification.

```
Downstream Packet Header, 20 bytes
  offset  len  field
       0   10  Session             alphanumeric, e.g. "SAMPLE0000"
      10    8  Sequence Number     big endian
      18    2  Message Count       number of Message Blocks
      20   ..  Message Blocks

Message Block
  offset  len  field
       0    2  Length              big endian, EXCLUDING these two bytes
       2   ..  Message data
```

A Message Block is **byte-identical to an ITCH frame**: the same two-byte
length prefix in front of the same body. That is why a block can be
handed to a decoder with no adjustment at either layer, and why the
length read while framing is the length the decoder reads again.

Three things about this format that are easy to get wrong:

- **There is no checksum.** MoldUDP64 is an unreliable transport
  wrapper and does not checksum packets. Integrity is SOUP's job, in the
  protocol layered above it. Implementing a CRC in the framing would mean
  inventing a field the specification does not define.
- **The Sequence Number is eight bytes**, not four.
- **A heartbeat packet** (Message Count 0) carries the next expected
  sequence number and no messages. It is part of the transport, not the
  feed, and carries no frame.

### 2.1 Walking the capture

```
pos = 0
while pos < size:
    if size - pos < 20:            -> the capture ends mid-header; stop
    count = be16(capture[pos + 18])
    pos += 20
    if count == 0xFFFF:            -> end of session; stop
    if count == 0:                 -> heartbeat; no frames; continue
    for i in 0 .. count-1:
        if size - pos < 2:         -> the capture ends mid-prefix; truncated frame
        length = be16(capture[pos])
        pos += 2
        if size - pos < length:    -> the capture ends mid-body; truncated frame
        frame = capture[pos .. pos+length]   (with the two prefix bytes)
        pos += length
        yield frame
```

The count in the header is trusted. A packet whose blocks run out
before its count is exhausted is a **truncated capture**, not a shorter
packet, and the missing message is reported as a truncated frame rather
than quietly dropped.

---

## 3. The trace

A trace file is a text file. The first line is a comment:

```
# hft-rtl-trace v1
```

Every subsequent line is one **record**, one per frame in the capture,
in order:

```
<index> <status> <type> <length> <field> <field> ...
```

| token | meaning |
|---|---|
| `index` | position of the frame in the capture, from zero, in decimal |
| `status` | one character, from the table below |
| `type` | the message tag byte as two lowercase hex characters, or `--` |
| `length` | the declared body length in decimal, zero-padded to four digits |
| `field` | one field value, in wire order, space-separated |

The length is zero-padded so that a change in its width cannot change
the line's shape: a reader that splits on whitespace sees the same
tokens in the same places whatever the length is.

### 3.1 Status

| status | meaning | type | length |
|---|---|---|---|
| `D` | decoded | the tag | the declared length |
| `S` | skipped: a well-formed frame of a type this build does not decode | the tag | the declared length |
| `L` | bad length: the declared length disagrees with the type | the tag, if it was read | the declared length |
| `M` | malformed: a field held a value outside its defined range | the tag | the declared length |
| `T` | truncated: fewer bytes were available than the length prefix promised | `--` | the declared length, or `0000` |

### 3.2 When the tag is recorded, and when it is not

This is load-bearing, not cosmetic. The decoder reads the tag **only
after it knows the whole body is present**, so the rule is:

- fewer than two bytes available for the length prefix → `T`, no type
- the declared body length is zero → `L`, **no type** (there is no tag
  byte on the wire)
- fewer bytes available than the declared length requires → `T`, **no
  type** (the body is incomplete, so the decoder stopped before the tag)
- otherwise the tag is read, and the record carries it

The two `L` cases are the same status with different type presence, and
the `--` token is what distinguishes them. Two implementations that
disagree here disagree about **where the decoder draws the line between
"not enough data yet" and "this frame is wrong"**, which is a real
difference and not a formatting one.

### 3.3 The decode decisions, in order

Once the tag is read:

1. If the tag is a type this build decodes and the declared length is
   not that type's length → `L`.
2. If the tag is a type this build does not decode → `S`.
3. If the message is keyed on an order reference and that reference is
   zero → `M`.
4. If the message is an Add Order whose side byte is neither `B` nor
   `S` → `M`.
5. Otherwise → `D`, with the fields.

Rule 3 applies to the order-level messages only (`A`, `E`, `C`, `X`,
`D`, `U`). It does **not** apply to `P` Trade, where the order reference
is populated with zero by design on the binary feeds, nor to `B` Broken
Trade, where the field is a match number, nor to `Q` Cross Trade, where
the field is an eight-byte share count that the specification says may
be zero when interest is insufficient to cross.

### 3.4 Field values

A field value is rendered as **twice the field's width on the wire**
lowercase hex characters, most significant byte first. ITCH is big
endian, so a 4-byte share count of 100 is `00000064` on the wire and is
rendered `00000064`.

The fields are in **wire order** — ascending offset — and only a `D`
record carries any.

| type | fields (name, bytes on the wire) |
|---|---|
| `A` Add Order | locate 2, tracking 2, timestamp 6, id 8, side 1, shares 4, stock 8, price 4 |
| `E` Order Executed | locate 2, tracking 2, timestamp 6, id 8, shares 4, match 8, printable 1 |
| `C` Order Executed At Price | locate 2, tracking 2, timestamp 6, id 8, shares 4, match 8, printable 1, execution_price 4 |
| `X` Order Cancel | locate 2, tracking 2, timestamp 6, id 8, shares 4 |
| `D` Order Delete | locate 2, tracking 2, timestamp 6, id 8 |
| `U` Order Replace | locate 2, tracking 2, timestamp 6, original_id 8, new_id 8, shares 4, price 4 |
| `B` Broken Trade | locate 2, tracking 2, timestamp 6, match 8 |
| `P` Trade | locate 2, tracking 2, timestamp 6, id 8, side 1, shares 4, price 4, match 8 |
| `Q` Cross Trade | locate 2, tracking 2, timestamp 6, shares 8, price 4, match 8, cross_type 1 |

Two of these lists are shorter than the wire, and deliberately so:

- **`P` Trade has no stock symbol.** The symbol occupies offsets 24..31,
  between the share count at 20 and the price at 32. The decoder steps
  over it and does not carry it, so the trace does not record it.
- **`Q` Cross Trade has no stock symbol.** The symbol occupies offsets
  19..26, between the eight-byte share count at 11 and the price at 27.
  Same reasoning.

The step is still covered by the differential. An implementation that
read the Trade price at offset 24 instead of 32 would put the symbol
bytes in the trace where the price belongs, and the comparison would
fail. What is *not* covered is an implementation that skipped the wrong
number of bytes and happened to produce the same price — which is not a
thing a decoder can do.

### 3.5 The 48-bit timestamp

ITCH carries the timestamp as **two bytes of high bits followed by four
bytes of low bits**. Reading it as a contiguous six-byte integer, or as
`high * 65536 + low`, both produce plausible timestamps that reorder a
book. The correct reassembly is `high * 2^32 + low`, and the trace
records the result as a six-byte value — twelve hex characters.

### 3.6 A worked example

An Add Order with stock locate 1234, tracking 0, timestamp
0x1F1ACED9F07B, order reference 42, side `B`, 100 shares, stock symbol
`SIMTEST `, price 1502500, is:

```
0 D 41 0036 04d2 0000 1f1aced9f07b 000000000000002a 42 00000064 53494d5445535420 0016ed24
```

and the same frame in a feed, read by an implementation that got the
shares and the price the wrong way round, is:

```
0 D 41 0036 04d2 0000 1f1aced9f07b 000000000000002a 42 0016ed24 53494d5445535420 00000064
```

which differs, in fields 5 and 7, in the exact way the bug did.

---

## 4. The hex sidecar

The RTL testbench has to read the capture, and reading a binary file in
VHDL is a known friction point. The C++ side therefore writes the
capture out as a **hex sidecar**:

- lowercase hex, two characters per byte
- 64 bytes (128 characters) to a line
- no header, no separators, nothing but hex and newlines
- every byte of the capture, in order, and nothing added

It is a faithful dump, so the RTL side is provably looking at the same
bytes the C++ side did. A human can read it when a comparison fails.

---

## 5. Comparing two traces

Records are compared **positionally**: field 0 against field 0, field 1
against field 1, and so on. The field names the comparison prints are a
convenience for a human reading a failure; they are not part of the
format and are not read from the trace.

The comparison reports:

- a difference in the number of records
- a difference in a record's index, status, type, or declared length
- a difference in a record's field count
- a difference in a field's value

and exits non-zero if any of them is found.

---

## 6. What this format does not cover

- **The book.** The trace records what the decoder extracted from each
  message, not what the book did with it. A second implementation of the
  order book is a separate exercise, and its comparison is a book-state
  checksum rather than a trace.
- **Timing.** The trace carries no clock reading. Cycle-exact
  measurement is a different artefact, produced by the RTL model itself
  and not by this format.
- **Transport behaviour.** Sequence gaps, heartbeats and end-of-session
  are handled by the walk in §2.1 but are not recorded in the trace. A
  gap is a property of a capture, not of a frame.


# 004 — ITCH decoder

**Status:** Agreed · **Updated:** 2026-10-26

## Proposal

Component 4 of note 001: the decoder that turns one TotalView-ITCH 5.0 message's bytes into its
decoded form, the structs of note 003 (D1) that the handler takes. Note 003 built a **reference
decoder** for clarity. This note builds the fast one, holds it to the reference over every message
of the sample day, and measures both, alone and with the handler behind them.

It does not frame (note 007 splits MoldUDP64 packets and the recorded files into messages) and
does not check sequence (note 005). Its input is one message's bytes and length; its output is a
`message`, or a refusal.

## Design

**What the work is.** Every field is at a fixed offset given by the message type; integers are
big-endian, 2, 4, 6 or 8 bytes; the rest is copied as is. So decoding is choosing a layout by the
first byte, checking the length, then loading and byte-swapping a few fields. There is nothing to
search, unlike FIX. The cost is in the dispatch on the type, the unaligned loads, the swaps, and
writing a 64-byte struct. On the sample day, three types are 96% of messages: Add (`A`, 44%),
Delete (`D`, 43%) and Replace (`U`, 9%).

**Decisions** (each with what is proposed):

- **D1. How fields are read.** Built three ways and measured, each producing exactly the reference
  decoder's `message`:
  - *A.* scalar: one fixed-width unaligned load per field (`memcpy` into an integer of its width)
    and a byte swap (`__builtin_bswap*`; a 6-byte timestamp as an 8-byte load masked), one
    function per type, chosen by a `switch` on the type;
  - *B.* the same loads, chosen by a table of 256 function pointers indexed by the type byte, so
    the dispatch is one indirect call instead of a branch the predictor must learn;
  - *C.* SIMD: the message loaded into vector registers and its bytes permuted by a mask chosen
    per type (`vpshufb`), so each field arrives byte-swapped where note 003's struct holds it.
    The shuffle moves bytes only within a 16-byte lane, so a field whose source and place lie in
    different lanes needs a further step; the struct stays note 003's (D1 there), so how many
    such steps each type needs decides whether C can win at all against A's three to six
    loads. x86-64-v3 only; other architectures use A.
  *Proposed:* all three built and measured; the default chosen from the figures.
- **D2. Reading past the end.** C's 32-byte loads, and A's 8-byte load of a 6-byte timestamp near
  a message's end, read bytes beyond the message. *Proposed:* as the gateway's note 009, a
  **padded-input contract**: the caller guarantees at least 32 readable bytes after each
  message's end (the receive buffers of note 007 are allocated so; a test feeds messages at the
  very end of a page followed by an unmapped page, to show what breaks without it). Each
  alternative also gets an unpadded entry that never reads past the length, for callers without
  the guarantee, measured too.
- **D3. What is checked.** *Proposed:* the type (known or refused) and the length (exactly the
  type's, or refused), nothing more: field values (a side other than B or S, a zero share count)
  are the book's and the handler's to judge (note 002, D5), and checking them twice costs every
  message. The same refusals as the reference decoder's.
- **D4. Correctness.** *Proposed:* as for the book and the handler: every alternative equal to the
  reference decoder, field by field, on every message of the sample day; the reference
  decoder's hand-built cases run against each; and a libFuzzer target (clang, as the gateway's)
  feeding arbitrary bytes to every alternative and the reference, any difference in the result or
  the refusal a failure, under ASan for the unpadded entries.
- **D5. Measurement.** *Proposed:* each alternative and the reference decoder: the cost per message
  by type, each call timed, over the day's messages in order (the real mix and order of types,
  for the branch predictor's sake: the gateway's note 009 found predictor state moving
  decoding's figures); the whole day as messages a second; and decode plus the handler on the
  default book, end to end, against note 003's figures for the handler alone. Branch
  mispredictions per message counted, as the gateway's stage benchmark does.

## Spec

- S1. Every decoder alternative, padded and unpadded, produces the reference decoder's `message`
  for every message of the sample day, and the same refusal for every malformed input in the
  hand-built cases.
- S2. A libFuzzer run of at least an hour finds no difference between any alternative and the
  reference decoder, and no out-of-bounds read in the unpadded entries (ASan).
- S3. Decoding a message makes no allocation or system call (checked in the compiled code).
- S4. The padded entries read no further than 32 bytes past a message's end (a test at a page's
  end shows it); the unpadded entries read nothing past it.
- S5. The cost per message by type, mispredictions per message, the whole day's messages a second,
  and decode plus handling end to end are published for every alternative and the reference
  decoder, as ranges over at least three runs, via `run.py report`.

## Tasks

- [x] D1 A, B, C, padded and unpadded; the struct layout C needs, with the reference decoder still
      producing the same values
- [x] The hand-built cases against every alternative; the page-end test (S4)
- [x] The sample day: every alternative against the reference decoder (S1)
- [ ] The libFuzzer target and an hour's run (S2); S3's call-graph check
- [ ] The benchmark and the report (S5)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-26 | Draft | Written for review. The type mix from the sample day (note 002's Review): Add 44%, Delete 43%, Replace 9%, the rest 4% |
| 2026-10-26 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 three ways, the default from the figures; D2 a padded-input contract with unpadded entries beside; D3 type and length only; D4 the reference decoder as oracle, on the day and under libFuzzer; D5 by type in the day's order, mispredictions counted, and end to end with the handler |
| 2026-10-26 | Agreed (tasks) | **Built.** `decoder.hpp`: one table per type of its fields (wire offset and length, place in note 003's struct, how moved: copied, a big-endian integer, or a default the reference decoder sets), from which all three are made: A and B unroll it into straight-line loads and swaps (A chosen by a `switch`, B through 256 function pointers), C builds from it per type the `pshufb` masks taking each input chunk into each of the struct's four 16-byte lanes and OR-ing a lane of constants. The struct stays note 003's. **Found when built, D2:** A and B never read past a message (every field read at its width; the 6-byte timestamp as the low part of an 8-byte load beginning in the header), so they need no padded entry; only C reads past (up to 15 bytes: its last chunk), so C alone has two entries. `decoder_test`: every type with every byte distinct and a million random messages (80% valid, the rest refused for type or length) through every alternative against the reference decoder, compared through a description of every field written apart from the tables (`tests/message_text.hpp`); shown to catch one wrong offset put in on purpose. S4: every type ending at an unmapped page: A, B and C unpadded decode; C padded faults (in a child process; under ASan, its handler reports it); with 32 bytes after every type it does not. Clean under ASan/UBSan and TSan (the fault check first missed ASan's way of reporting a fault: an error exit, not death by the signal; fixed). **S1:** `decoder_day_test`, every alternative equal to the reference decoder on all 423,285,709 messages of the day (681 s). **S3:** `check_hot_path`, every alternative: only memory functions and the stack protector; B's one indirect call is through its own table, whose 23 targets are followed as if called. A check run on the day's first 640 MB, outside bench mode: reference 68.7 M messages a second, A 72.6, B 80.6, C padded 43.5, C unpadded 35.5; with the handler 6.8–8.3 M: decoding is about a tenth of the work, and C is slower than the byte loads, as the Draft cautioned |

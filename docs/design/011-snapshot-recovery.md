# 011 — Recovery from a snapshot in the program

**Status:** Agreed · **Updated:** 2026-10-29

## Proposal

Note 001's recorded follow-up: the program `itchfh` (note 009) has no snapshot path. When the
sequencer gives up on a gap (`lost`: its retries spent or its hold full, note 005's D4), the
program waits, reported unhealthy, though recovery from a snapshot is proven in tests (note
006's S4, note 008's S2). This note puts that recovery into the program, against a GLIMPSE 5.0
server of our own over SoupBinTCP on a real socket, and measures what a recovery costs at the
day's worst point.

The pieces exist: note 006's server and client (sans-I/O), the sequencer's `resume`, and the
publisher's `mute`, `rebind` and `restored` (note 008's D5). What is new is everything around
them: a TCP server, a client in the program's loop, where the rebuilt book comes from, and what
happens when recovery itself fails.

What the record shows, before any design:

- **Building the fresh book is not in note 006's figure.** Applying the spin at the peak of live
  orders took 362–369 ms into a book already built; building one (the default book, ~2.5 GB,
  faulted in at construction) is not counted. In the program it would be.
- **Note 006's client decodes with C, not B.** Its D4 says "the same decoder and handler as the
  live feed"; it was built with `decode_simd_unpadded` (C, the slowest in note 004), while the
  live path uses `decode` (B, note 004's default). So its published apply figure is C's.
- **The program has one thread** (note 007's D1). An apply that runs to the end leaves the
  multicast socket unread for its whole length.
- **The server's build is on demand:** 948–1,215 ms on the reference structures (note 006),
  while the feed keeps arriving.

## Design

```mermaid
sequenceDiagram
  participant S as feed_sender (feed + snapshot thread)
  participant P as itchfh (one thread)
  participant C as consumers
  P->>P: sequencer lost; the feed held
  P->>C: feed lost
  P->>S: SoupBinTCP login (TCP)
  S->>S: snapshot at N, between two sent messages
  S->>P: spin, then End of Snapshot (N)
  P->>P: spin applied in slices into the standby book, the sockets read between slices
  P->>P: swap books; resume(N); the held feed from N
  P->>C: reset, the rebuilt books replayed, then live
```

**Decisions** (each with what is proposed):

- **D1. When to recover.** *Proposed:* whenever the sequencer is `lost` (retries spent or hold
  full), and also at start with `join = snapshot` in the configuration, for a program started
  mid-session (default `join = start`: re-requests from message 1, as now). A late join and a
  loss are the same path. *Alternative:* `lost` only.
- **D2. The snapshot server.** *Proposed:* a thread in `feed_sender`, on a CPU of its own,
  following the sender's session store up to the last message sent (one atomic counter between
  them) with note 006's server unchanged (D1 there: the reference structures), and serving
  SoupBinTCP on a TCP port beside the control port (26403). It stands in for Nasdaq's GLIMPSE
  service as the sender stands in for the feed, and it crosses the card in the card's namespace
  as the feed does. A login is answered with a snapshot at the point the thread has reached:
  between two messages, so the End of Snapshot's number is right by construction (note 006, D2).
  *Alternative:* a process of its own that joins the multicast and sequences it like any
  receiver. Closer to a real deployment, but it needs its own recovery, and the result would be
  the same.
- **D3. Where the snapshot goes.** *Proposed:* a standby book and handler, built and faulted in at
  start on the same node (memory is not short: 96 GB a node). The spin is applied into the
  standby. At End of Snapshot they are swapped in: the publisher rebound (`rebind`, `watch`), the
  feed's handler pointed at them, `restored()`. The old pair is rebuilt fresh on a housekeeping
  thread and becomes the next standby. A second loss before it is ready waits for it, and that
  is counted. *Alternative:* clear the live book in place and apply into it. One book, but the
  clear is either a pass over gigabytes or page faults during the apply, on the program's thread.
- **D4. One thread, the spin in slices.** *Proposed:* the snapshot connection is a non-blocking
  socket read in the program's loop. Each pass applies at most a bounded slice of the spin
  (64 KB, about 1,600 messages, ~0.3 ms at note 006's rate), then reads the multicast and request
  sockets as usual, so live packets go into the sequencer's hold, not the kernel's buffer. This
  keeps note 007's rule: one thread never waits on anything. Why it matters: at the day's
  busiest second (~16k packets), 0.4 s unread is ~6,400 packets, ~9 MB, past the 4 MB socket
  buffer, and those drops would need re-requests, which is what failed. *Alternative:* apply in
  one go with a larger socket buffer (sized for the worst apply, and the apply's length
  unbounded), or on a second thread (the handler then has two writers, or a hand-over).
- **D5. A hold sized for a recovery.** *Proposed:* the default `hold_mb` raised from 64 to 256.
  A recovery at the peak is about 1.7 s: the server's build ~1.2 s, the transfer, the apply
  ~0.4 s. At the day's busiest 100 ms rate (~25k packets a second), that is ~43k packets held,
  and 64 MB holds 32k (2 KB slots). 256 MB holds 131k, ~5 s at that rate. If the hold still
  fills, the snapshot is applied anyway, and what came after the hold filled is a gap from N on,
  re-requested as usual, then another snapshot if that fails. *Alternative:* keep 64 MB and
  accept a second recovery at the worst moments.
- **D6. The same decoder as the live feed.** *Proposed:* the client decodes with `decode` (B), as
  note 006's D4 said. Note 006 is amended: its Review records the change, and its snapshot set is
  re-measured with this note's runs and republished, so its apply figure is B's.
  *Alternative:* leave C, and record the difference.
- **D7. When recovery fails.** *Proposed:* the server cannot be reached, refuses the login, or
  drops the connection mid-spin; or the spin is malformed (undecodable, or no End of Snapshot).
  Then the standby is discarded (rebuilt, as in D3), the feed stays `lost` and unhealthy, and
  the program tries again after a second. A spin is never applied in part. The monitoring block
  gains `snapshots`, `snapshot_failures`, `last_recovery_ns` and a `recovering` health bit, with
  its version raised.
- **D8. Correctness.**
  - In process, over loopback sockets: recovery from each cause of `lost` (retries spent, hold
    full), a late join, and each failure of D7. Then the books equal a book fed every message,
    and the consumers' books equal the program's.
  - Through the programs (label `data`): `feed_sender` gains a control command that loses a range
    on purpose (drops those packets and refuses requests for them). The day at full speed through
    `itchfh` with a loss at the peak of live orders: one recovery, every consumer's books at the
    close equal a book fed the day directly, and the feed `live` afterwards.
  - ThreadSanitizer on the standby's rebuild (D3).
- **D9. Measurement.** *Proposed:* recovery through the programs at the day's own pace, from just
  before the peak of live orders (note 006: message 412,349,206), on loopback and the card with
  busy polling. A loss forced there, timed from `lost` to `live` with the hold drained, in three
  parts: the request to End of Snapshot received (the server's build and the transfer), the
  apply, and the backlog. Also the spin's bytes, the packets held, kernel drops during recovery
  (expected none, D4), and the live feed's kernel-to-handled latency during recovery against
  without. At least three runs, as ranges, bench mode.

## Spec

- S1. A `lost` feed in the program is recovered from a snapshot without a restart, and with
  `join = snapshot` a late start joins from one. Afterwards the books equal a book fed every
  message, and every consumer's books equal the program's (in process, and the whole day through
  the programs).
- S2. Each failure of D7 leaves the feed `lost` and unhealthy, the old books untouched by any
  part of a spin, and a retry follows. Each is counted in the monitoring block.
- S3. During a recovery the multicast socket is read at least once per slice (D4). At the peak, a
  recovery causes no kernel drops on loopback.
- S4. The client decodes with the live path's decoder (D6), and note 006's snapshot set is
  republished from runs at this note's commit.
- S5. Recovery time at the peak of live orders, in D9's three parts, with the packets held, is
  published as ranges over at least three runs on each path, via `run.py report`.
- S6. The program's hot path still passes `check_hot_path` (the snapshot slice included, its
  socket call allowed), and allocates nothing in steady state, recovery included (the standby's
  rebuild is on its own thread).

## Tasks

- [ ] `feed_sender`: the snapshot thread and SoupBinTCP over TCP (D2); the deliberate loss
- [ ] The program: the snapshot client in the loop, in slices (D4); standby and swap (D3); D1's
      `join`; D5's hold; D7's failures and monitoring
- [ ] The client on `decode` (D6); note 006 amended
- [ ] Tests: in process, failures, the day through the programs (D8)
- [ ] The benchmark and report (D9); note 006's set re-measured

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-29 | Draft | Written for review from the record. Found while reading it: note 006's client decodes with C (`decode_simd_unpadded`), not the live path's B, against its D4; its apply figure leaves out building the fresh book; the server builds on demand (~1.2 s at the peak) while the feed keeps arriving, so the hold must cover a recovery (64 MB holds ~32k packets, a recovery at the peak needs ~43k); and no GLIMPSE server exists over a socket yet |
| 2026-10-29 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 recover whenever `lost`, and `join = snapshot` for a late start; D2 the server a thread in `feed_sender` following what has been sent, SoupBinTCP on 26403; D3 a standby book and handler, swapped in, the old rebuilt on a housekeeping thread; D4 the spin in 64 KB slices between socket reads; D5 `hold_mb` 256; D6 the client on `decode` (B), note 006 amended and its set re-measured; D7 failures leave the old books, retry after a second, counted; D8 tests in process, through the programs, TSan; D9 recovery at the peak, three parts, both paths |

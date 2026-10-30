# 011 — Recovery from a snapshot in the program

**Status:** Built, runs published · **Updated:** 2026-10-30

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

- [x] `feed_sender`: the snapshot thread and SoupBinTCP over TCP (D2); the deliberate loss
- [x] The program: the snapshot client in the loop, in slices (D4); standby and swap (D3); D1's
      `join`; D5's hold; D7's failures and monitoring
- [x] The client on `decode` (D6); note 006 amended
- [x] Tests: in process, failures, the day through the programs (D8)
- [x] The benchmark and report (D9); note 006's set re-measured

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-29 | Draft | Written for review from the record. Found while reading it: note 006's client decodes with C (`decode_simd_unpadded`), not the live path's B, against its D4; its apply figure leaves out building the fresh book; the server builds on demand (~1.2 s at the peak) while the feed keeps arriving, so the hold must cover a recovery (64 MB holds ~32k packets, a recovery at the peak needs ~43k); and no GLIMPSE server exists over a socket yet |
| 2026-10-29 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 recover whenever `lost`, and `join = snapshot` for a late start; D2 the server a thread in `feed_sender` following what has been sent, SoupBinTCP on 26403; D3 a standby book and handler, swapped in, the old rebuilt on a housekeeping thread; D4 the spin in 64 KB slices between socket reads; D5 `hold_mb` 256; D6 the client on `decode` (B), note 006 amended and its set re-measured; D7 failures leave the old books, retry after a second, counted; D8 tests in process, through the programs, TSan; D9 recovery at the peak, three parts, both paths |
| 2026-10-30 | Agreed (building) | **Built** (`30994d3`, CI green; the fixes below since). `snapshot_link.hpp`: `glimpse_service` (on its own thread: follows a session store up to the sender's next message, published by `mold_sender` after each packet; a login answered with a spin after catching up to that point; faults on demand for tests: refuse, cut, corrupt) and `snapshot_connection` (non-blocking connect, the Login Request built once, reads of at most a slice). `recovery.hpp`: `published_on` (a book's handler sharing one publisher) and `recovery` (D1 `lost` or `join`; D3 two pairs built at start, swapped through `feed_one::replace_handler`, `rebind`, `watch`, `restored()`, the old pair rebuilt on a thread pinned to `rebuild_cpu`; D4 `step` a slice at a time, `drain` reading the sockets between; D7 a failure closes, unmutes, rebuilds the standby if any of a spin went in, retries after `retry_ns`; no progress for `timeout_ns` is a failure). The program: `join`, `snapshot`, `snapshot_port` (0: none), `rebuild_cpu`, `hold_mb` 256; monitoring block v3 (`snapshots`, `snapshot_failures`, `snapshot_waits`, `last_recovery_ns`, `recovering`). `feed_sender`: the service on the request port + 2 given a CPU; the R command's longest form loses a range on purpose (packets not sent, requests touching it refused); a mid-day run waits until the service has followed that far. **Found while building:** (1) the publisher's table followed a spin while muted, so a failed spin left a trace and a successful one left an instrument the spin no longer had with its old entry: note 008's D5 amended (mute leaves the table, `restored()` rebuilds it); (2) the SoupBinTCP reader grew a vector (an allocation reachable from the slice, S6): rewritten to parse in place, a split packet's tail in a fixed buffer; (3) the sequencer applied packets arriving in order while `lost` (a late join with the feed flowing: into books about to be replaced, and `resume` then moved the next expected back, so everything from N was asked for again and came ~1 s late): now held while lost (note 005, Review); (4) the corrupt fault must hit a message's type byte (a flipped price byte decodes cleanly); (5) our service follows on note 006's reference structures, ~1.2 million messages a second, so a run from mid-day first waits minutes, and D8's day is sent at 15k packets a second (~0.65 million messages), not at full speed. **Tests.** `recovery_test` (in process): retries spent, hold full, a late join, lost while in order, and S2's five failures in turn (the connection refused, a service that never answers, the login refused, the spin cut short, the spin corrupt: each leaves the feed lost and the live book and table unchanged); the table emptied by the rebuild, directly. Mutants checked: no table rebuild, no mute guard on the table, packets applied while lost: each caught. TSan and ASan/UBSan clean. `check_hot_path` S6: the program's pass and a slice (recv, close, clock_gettime besides memory; B's table). `recovery_day_test` (label `data`): passes, 13.4 min: 45 packets lost on purpose, 96 requests refused, one snapshot, none failed, recovery 2.29 s, healthy at the end, the consumer's books at the close equal a book fed the day directly. It also had 2,642 kernel drops (recovered by re-request): at 15k packets a second ~2.3 s of feed is held, and `resume` delivers it in one call, longer than the socket's buffer lasts. **Trials** of `recovery_bench` over loopback (not bench mode; the first two exposed benchmark flaws: the join asked while the service was still following the day, timing out 13 times, and the consumer counted nothing while replayed, when the held messages arrive): a loss at the peak 1.60 s from `lost` to `live` (waiting for the spin 1.28 s, the service's build; applying 300 ms; the backlog 25 ms, 2,030 packets), no kernel drops; the join 1.58 s. The peak of live orders is not the busiest time: ~1.3k packets a second there, so S3 holds at the peak; at the day's busiest second (~16k) the backlog would be ~12 times larger |
| 2026-10-30 | Agreed (building) | **Trials since the fixes** (loopback, not bench mode, four runs): the loss at the peak 1.60–1.67 s from `lost` to `live` (waiting for the spin 1.28–1.34 s, applying 300–304 ms, the backlog 25–26 ms, ~2,030–2,060 packets held), the join 1.58–1.79 s, no kernel drops, none failed. The standby's rebuild takes ~1.72 s after each recovery, on CPU 1 (now timed by the recovery and recorded in each run). A suspicion checked and dropped: two trials of four had a millisecond tail in the steady stretch before the loss (p99.9 3.3 ms), suspected to be the rebuild (3 GB unmapped and faulted in the same process); a trial with the rebuild switched off had a 4 ms maximum too, and the timed trial showed the rebuild ending 1.5 s before the stretch begins, its maximum 70 µs: run-to-run noise outside bench mode, not the rebuild. The benchmark's 'before the loss' starts 5 s after the start (the join, its backlog and its rebuild past). For Shep: the bench-mode runs (three a path) and note 006's `snapshot` set re-measured |
| 2026-10-30 | Agreed (building) | **The first bench-mode check run** (`results/recovery-dev-lnx01-20261030T120600Z.json`, `e253410`) had a bad noise block: every reserved CPU stopped ~3% (1,830 stops a second of ~17 µs, CPUs 2, 3 and 6 alike), no SMIs (turbostat: none), clean with the sender stopped. The sender's new service thread slept 1 ms at a time before a run and spun on `accept()` after; every wake disturbed the reserved CPUs. Fixed (`cc4673a`): the service waits on the run counter (a futex) before a run and in `poll()` for a login once caught up; the standby's rebuild thread waits on a futex for work, the end of a spin waking it (`check_hot_path` allows that one system call in the slice). A noise check with the fixed sender: no stops on CPUs 2, 3 and 6. The run is withheld (not in the manifest, removed from `results/`) |
| 2026-10-30 | Built (runs) | **Published:** over loopback `results/recovery-dev-lnx01-20261030T125049Z.json`, `…130208Z`, `…130937Z`; over the card with kernel busy polling `…132532Z`, `…133255Z`, `…134023Z` (`cc4673a`, clean, bench mode, noise blocks clean; every run whole: two snapshots, none failed, the consumer's books whole). A loss at the peak of live orders: 1,542–1,546 ms from `lost` to `live` over loopback, 1,554–1,559 over the card; of it, waiting for the spin 1,254–1,259 ms (our service building on demand on note 006's reference structures, ~1.2 s, not the program), applying 264–276 ms, the held feed delivered in 24 ms (1,995–1,999 packets held), no kernel drops. The join alike (1,530–1,546 ms). The standby rebuilt 1,383–1,389 ms after each recovery, on CPU 1. Latency, intended send to read: before the loss p50 9.5–9.7 µs loopback, 23.1–23.4 µs the card, max 39–63 µs; the 2 s of day time from the loss p99 1,311–1,330 ms, max 1,518–1,535 ms (held through the recovery, as expected). Applying in the program (264–276 ms) costs 34–46 ms more than note 006's client alone (230–231 ms, re-measured at this commit): the slicing, the socket reads between slices and the table rebuilt by `restored()`, a guess not measured apart; the card's extra 10 ms fits busy-polled reads between slices. **Against the Spec:** S1 met (in process; the day through the programs, `recovery_day_test`; every run's consumer whole); S2 met (`recovery_test`, five failures, each counted); S3 met (one read per slice by D4, no kernel drops at the peak on either path); S4 met (B in the client, note 006's set republished from `cc4673a` runs); S5 met (three runs a path, `run.py report`); S6 met (`check_hot_path`, with the futex wake allowed). **Open for Shep:** (1) D8's day test is sent at 15k packets a second, not the day's own pace or full speed, and at that pace it had 2,642 kernel drops: the backlog after a recovery is delivered in one call (chunked delivery not built); (2) the peak of live orders is a quiet time (~1.3k packets a second): at the day's busiest second (~16k) a recovery would hold ~12 times as much; (3) note 008's publish set was not re-measured after the mute change (a branch on every publish); (4) the waiting part is our test service's build: a real GLIMPSE server answers from state it keeps, so it is not the program's figure |

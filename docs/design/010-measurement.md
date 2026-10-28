# 010 — Measurement

**Status:** Agreed · **Updated:** 2026-10-27

## Proposal

Note 001's third aim and its S5: the handler measured end to end, as `lockfree-ipc` and the
gateway were. Latency from a packet to the published update it causes, as percentiles, open
loop; throughput over the whole day; both beside the reference implementation on the same
harness. Notes 002–009 measured each component alone (the book, handler, decoder, sequencer,
snapshots, receiving, publishing, recording); this note measures them together, as the program
of note 009 runs them, and says what the whole adds up to.

It does not tune anything: a finding here that calls for a change goes back to the component's
note.

## Design

```mermaid
flowchart LR
  S[feed_sender: intended send time per message] -->|multicast| R[receive, sequence, decode, handle]
  S -.->|stamps, shared memory| C
  R --> P[publish] --> C[consumer process: read time]
```

**Decisions** (each with what is proposed):

- **D1. What "packet to published update" is.** *Proposed:* from the time the sender meant to
  send a packet to the time a consumer, in its own process on the publisher's node (note 008's
  amendment), reads an update one of its messages caused. One way, which this machine allows:
  sender and consumer share the invariant TSC (`lockfree-ipc`'s note 001), and a shared-memory
  array of stamps by sequence number (a network namespace does not separate `/dev/shm`, so the
  card path works the same). The sender writes each message's stamp before it sends the packet;
  the consumer reads it when the update arrives. *Alternative:* note 007's kernel receive stamp,
  which leaves out the sender's kernel, the loopback or the card, and the wire.
- **D2. Open loop.** *Proposed:* the stamp is the intended send time on a fixed schedule, not
  the time the sender got to it, so a late sender or a backed-up receiver shows in the latency
  rather than slowing the load (coordinated omission: `lockfree-ipc`'s note 003). The sender's
  own lateness is recorded beside it.
- **D3. Paces.** *Proposed:* the first 50 million messages at fixed paces of 10k, 25k and 50k
  packets a second (the day's busiest second is about 16k, its busiest millisecond 35k: note 007);
  and the day's own pace for five minutes from 09:29:30, the open, so its real bursts are in it
  (the sender gains a starting message for this).
- **D4. Paths.** *Proposed:* loopback, and the card with kernel busy polling (note 007's
  recommended setting there).
- **D5. What runs.** *Proposed:* the components as `itchfh` runs them (note 009's D7: one thread,
  `recvmsg`, the default book, the publisher), in the benchmark's process so it can be pinned
  and timed as the other benchmarks are, with recording on (as deployed) and off. The consumer
  only reads and stamps, so its own work is not counted. *Alternative:* spawning `itchfh` itself,
  which measures the same code but could not run the harness's checks inside it.
- **D6. Throughput over the day.** *Proposed:* the day from a recording (note 009's replay) at
  full speed through the sequencer, decoder and handler, as messages a second: the fast handler
  on the default book, with the publisher and one consumer and without, and the reference
  handler on the reference book (it has no publisher). No network: the kernel's part is
  measured by note 007.
- **D7. The measurement's own cost.** *Proposed:* stated: the sender's stamp writes (one a
  message) and the consumer's clock read (one an update), each measured on its own.
- **D8. The report.** *Proposed:* one table of latency by path and pace (p50, p99, p99.9, max,
  the sender's lateness), one of the open, one of throughput, and a README summary: the headline
  latency at 25k packets a second on each path and the day's throughput.

## Spec

- S1. Packet to published update, open loop, is published as percentiles (p50, p99, p99.9, max)
  for each path and pace, as ranges over at least three runs, with the sender's lateness; in
  every run every message is handled once, no consumer is overrun or on another node.
- S2. The same for the open at the day's own pace.
- S3. The day's throughput through the sequencer, decoder and handler is published as messages
  a second, fast with and without the publisher and the reference implementation, as ranges
  over at least three runs.
- S4. The measurement's own cost (the stamp writes and the clock read) is measured and stated
  beside the figures.
- S5. Every run carries the harness's environment block (machine, kernel, pinning, NUMA, clock,
  noise), and a run outside bench mode is refused for publication, as for every set so far.

## Tasks

- [x] The sender's stamps and starting message (D1–D3)
- [x] The benchmark: paces, the open, both paths, recording on and off (D3–D5)
- [x] Throughput over the day (D6); the measurement's own cost (D7)
- [x] The report and README summary (D8)
- [x] Bench-mode runs, published

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-27 | Draft | Written for review. What it draws on: the day's rates (note 007: busiest second ~16k packets, millisecond 35k); one thread with the publisher keeps up to ~80k packets a second (note 009); a consumer must be on the publisher's node (note 008's amendment); the card's interrupts deliver bursts, busy polling removes them (note 007) |
| 2026-10-27 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 one way, intended send time to a consumer's read, stamps in shared memory; D2 open loop, the sender's lateness beside; D3 10k, 25k and 50k packets a second over the first 50 million messages, and five minutes of the open at the day's pace; D4 loopback and the card with busy polling; D5 the program's components in the benchmark, recording on and off; D6 the day's throughput from a recording, fast with and without the publisher, and the reference; D7 the measurement's own cost; D8 the report |
| 2026-10-27 | Agreed (building) | **Built.** `mold_sender` gains a starting message, a stamp hook (each packet's messages and intended send time, before the send) and its own lateness (the most, and how many packets over 10 and 100 µs); `feed_sender` a longer run command (the day's own pace, ITCH times from and to, answered with the messages they resolve to) and a stamps array (`bench/stamps.hpp`: 4 million slots by sequence number, CLOCK_MONOTONIC, one clock for every process, shared across the card's namespace through `/dev/shm`); `bench/e2e_bench` (D1–D5, D7) and `bench/throughput_bench` (D6: the day's recording made once into the build directory, 13.4 GB, and replayed by every case); `run.py`'s two sections and a README summary. `feed_test`: a run from message 30,001 to 50,000 against a receiver expecting 30,001, every message stamped before its packet, none outside. Trials outside bench mode (not published): packet to published update p50 11.2–12.5 µs at 10k–50k packets a second (the in-packet wait in it: ~43 messages handled one after another at ~190 ns each with publishing), the open 13.2 µs p50, 76 µs p99.9; at 50k the tail reaches milliseconds (one thread ~60% busy with publishing); the stamp write costs no more than the clock pair timing it (43 ns), the clock read ~50 ns net; the day: fast 4.29 M messages a second (98.7 s), published to one consumer 2.58 M, reference 1.00 M |
| 2026-10-28 | Built (runs published) | **Nine bench-mode runs** (`0221b4d`, clean; the first of each set a check run, read before the others): packet to published update three times on each path (loopback `results/e2e-dev-lnx01-20261027T234335Z.json`, `…20261028T124019Z`, `…125138Z`; the card with busy polling `…001509Z`, `…120644Z`, `…121803Z`) and the day's throughput three times (`results/throughput-dev-lnx01-20261027T235611Z.json`, `…20261028T130400Z`, `…131534Z`); the machine rebooted between the check runs and the rest (the throughput recording read back into the page cache before its runs). Every case whole: every message handled once, the feed ended, no consumer overrun or on another node, nothing unrecorded. **Recording on:** loopback 25k packets a second p50 11.8–13.2 µs, p99 26.4–28.2, p99.9 271–326; the open at its own pace 13.3–14.5, 37.3–38.5, 67.8–69.4; the card with busy polling 25k 24.8–25.6, 39.7–40.8, 385–411; the open 26.9–28.1, 52.3–53.6, 77.3–79.0. The card adds ~12 µs one way at p50 (its passes and the 10GBASE-T PHYs, the gateway's note 005). At 50k the tail reaches tens of milliseconds on both paths (5,952 kernel drops in one card run, recovered). **The day:** fast 4.20–4.23 M messages a second (100.1–100.7 s), published to one consumer 2.52–2.58 M, reference 1.01 M (417.5–421.0 s): the fast path 4.2 times the reference. The measurement's own cost: a stamp write no more than the clock pair timing it (16 ns), a clock read ~18 ns net. **Open:** (1) in every case on both paths the maximum is ~1.7–2.1 ms, even at 10k with p99.9 under 52 µs: one stall, repeating, cause not found; (2) the in-packet wait (a message waits for those before it in its packet) is in every figure and not separated; (3) the loopback check run, before the reboot, ~1 µs higher at p50 than the two after; (4) at 50k the tail: one thread publishing is ~60% busy on average, so some stretches must cost more than the pace allows (the open, a guess). Against the Spec: S1 and S2 (the e2e runs), S3 (the throughput runs), S4 (the own-cost figures), S5 (every run's environment block; all from bench mode). For Shep's review for Measured |
| 2026-10-28 | Built (amendment) | **The ~2 ms maximum, found (agreed with Shep to look before closing): the benchmarks' own consumers.** Traced with diagnostic copies (scratch, not committed): every slower message logged in bench mode showed one episode a case, ~60 ms in, latency climbing steadily to 1.7 ms then draining, a queue, not a stall; split at the kernel's receive stamp, the sender and loopback stayed on time (2–3 µs) and the receiving thread fell behind; the messages there are 4,000 Market Participant Positions (`L`) at 03:07, ~50 a packet, which in process cost 135 ns each without a consumer and 2,994 ns with one, idle or reading, wherever in the ring, with the state table or without. **The cause:** each benchmark `fork()`ed its consumer after building the producer's book, handler and publisher, so their pages were copy-on-write: the producer's first write to each afterwards took a fault and a 4 KB copy (~3 µs), and each `L` updates the handler's participant table at a random place, a new page a message. Forked first, the stretch costs 147 ns. Not the feed handler: a real consumer is a program of its own, sharing no pages with it. **Amended:** `e2e_bench`, `throughput_bench` (its published case) and note 008's `publish_bench` fork their consumers before the producer allocates (consumers retry their attach for a minute while it builds). Checked outside bench mode: in the `L` stretch kernel to handled now at most 7.5 µs. The `e2e` and `throughput` sets (and note 008's `publish` set) to be re-measured at the fixed commit; the runs above are superseded |
| 2026-10-28 | Built (runs published, re-measured) | **Re-measured at `1f39862`** (clean, consumers forked first): packet to published update three times on each path (loopback `results/e2e-dev-lnx01-20261028T153846Z.json`, `…160227Z`, `…161346Z`; the card with busy polling `…170916Z`, `…172034Z`, `…173153Z`) and the day's throughput three times (`results/throughput-dev-lnx01-20261028T163225Z.json`, `…164357Z`, `…165528Z`), replacing the earlier sets. Every case whole. **Recording on:** loopback 10k p50 11.3–11.4 µs, p99.9 26 µs, max 65–79 µs (was ~2 ms); 25k 11.3–11.7, p99.9 26.7–27.3 µs (was 271–326); the open 11.7–12.4, p99.9 34.7–36.7 µs (was 68), max 437–467 µs; the card 10k 24.1–24.3, p99.9 37 µs; 25k 24.1–24.5, 38–39 µs; the open 25.1–25.4, 52.4–54.5 µs. **The day:** fast 4.23 M messages a second (100.0–100.2 s), published to one consumer 2.57–2.62 M (was 2.52–2.58), reference 1.01–1.02 M (fast 4.2 times the reference). So the copy-on-write faults had taken 1.5–2 ms off every maximum and much more off the tails. **Open:** at 25k a maximum of 5.6–6.0 ms in every run on both paths, and at 50k 11.2–11.6 ms (p99.9 1.4–2.9 ms), each the same run to run: a guess, not shown, a stretch of the day whose messages cost more than those paces allow (a capacity limit, not the benchmark); the in-packet wait not separated. For Shep's review for Measured, with note 008's re-measured set |

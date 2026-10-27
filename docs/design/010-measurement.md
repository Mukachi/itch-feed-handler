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
- [ ] Bench-mode runs, published

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-27 | Draft | Written for review. What it draws on: the day's rates (note 007: busiest second ~16k packets, millisecond 35k); one thread with the publisher keeps up to ~80k packets a second (note 009); a consumer must be on the publisher's node (note 008's amendment); the card's interrupts deliver bursts, busy polling removes them (note 007) |
| 2026-10-27 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 one way, intended send time to a consumer's read, stamps in shared memory; D2 open loop, the sender's lateness beside; D3 10k, 25k and 50k packets a second over the first 50 million messages, and five minutes of the open at the day's pace; D4 loopback and the card with busy polling; D5 the program's components in the benchmark, recording on and off; D6 the day's throughput from a recording, fast with and without the publisher, and the reference; D7 the measurement's own cost; D8 the report |
| 2026-10-27 | Agreed (building) | **Built.** `mold_sender` gains a starting message, a stamp hook (each packet's messages and intended send time, before the send) and its own lateness (the most, and how many packets over 10 and 100 µs); `feed_sender` a longer run command (the day's own pace, ITCH times from and to, answered with the messages they resolve to) and a stamps array (`bench/stamps.hpp`: 4 million slots by sequence number, CLOCK_MONOTONIC, one clock for every process, shared across the card's namespace through `/dev/shm`); `bench/e2e_bench` (D1–D5, D7) and `bench/throughput_bench` (D6: the day's recording made once into the build directory, 13.4 GB, and replayed by every case); `run.py`'s two sections and a README summary. `feed_test`: a run from message 30,001 to 50,000 against a receiver expecting 30,001, every message stamped before its packet, none outside. Trials outside bench mode (not published): packet to published update p50 11.2–12.5 µs at 10k–50k packets a second (the in-packet wait in it: ~43 messages handled one after another at ~190 ns each with publishing), the open 13.2 µs p50, 76 µs p99.9; at 50k the tail reaches milliseconds (one thread ~60% busy with publishing); the stamp write costs no more than the clock pair timing it (43 ns), the clock read ~50 ns net; the day: fast 4.29 M messages a second (98.7 s), published to one consumer 2.58 M, reference 1.00 M |

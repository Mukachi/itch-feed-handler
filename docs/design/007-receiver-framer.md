# 007 — Receiver and framer

**Status:** Agreed (D1 amended) · **Updated:** 2026-10-26

## Proposal

Components 1 and 2 of note 001: the feed's way in. The **receiver** owns the sockets and buffers:
it takes MoldUDP64 datagrams from a multicast group, sends re-requests over unicast and takes
their answers, and reads recorded input through the same path. The **framer** turns what arrives
into packets and messages, checking every boundary (MoldUDP64's blocks: `mold.hpp`, note 005;
SoupBinTCP's stream: `soup.hpp`, note 006; the recorded files' 2-byte lengths).

It also builds the other end: **our MoldUDP64 sender**, replaying a recorded day onto the group
at a chosen pace, with the re-request server (note 005, D6) answering on unicast. This is note
001's D1: we have no Nasdaq feed, so the live path runs against our own sender, over loopback and
over the machine's 10GBASE-T ports (the gateway's `card-link` setup: its two ports cabled
together, the far one in a network namespace).

This is where the work meets the kernel, so most of its decisions are measured.

## Design

```mermaid
flowchart LR
  S[sender: the day, paced] -->|multicast| R[receiver socket]
  R --> Q[sequencer, note 005]
  Q -->|request, unicast| RS[re-request server]
  RS -->|answer, unicast| R
  Q --> D[decoder, note 004] --> H[handler, note 003]
  F[recorded packets] --> Q
```

**Decisions** (each with what is proposed):

- **D1. Taking datagrams.** *Proposed:* a non-blocking UDP socket joined to the group, read with
  `recvmmsg` in batches (up to 64 datagrams a call), each into its own buffer with the decoder's
  padding after it (note 004, D2: at least 32 readable bytes past every message, given here by a
  buffer of 1,500 + 64 bytes a datagram). *Alternative, measured beside it:* `recvmsg`, one
  datagram a call. *Amended (agreed with Shep, option A):* re-requests go from a second socket,
  the request socket (unicast, any free port on the interface), so their answers come back on it,
  out of the multicast queue; while the sequencer is gapped it is read first, otherwise it is left
  alone. With one socket an answer waits behind every multicast datagram not yet read, so a
  receiver behind the sender gave up (see Review).
  *Chosen from the figures (agreed with Shep, option A):* `recvmsg`, a datagram a call, the
  default; `recvmmsg` kept as the alternative (10k packets a second more before kernel drops, but
  0.3–1 µs slower at every pace the sample day reaches).
- **D2. Waiting.** *Proposed:* busy-polling the socket on the receiving CPU (the bench CPUs are
  reserved), with the kernel's own busy polling (`SO_BUSY_POLL`) measured on the card path, as the
  gateway did (its note 005, D2; over loopback it does nothing).
- **D3. Not losing datagrams in the kernel.** A full socket buffer drops datagrams silently; the
  sequencer then sees a gap and re-requests. *Proposed:* the buffer as large as the machine allows
  (`net.core.rmem_max` is 4 MB here: a few thousand datagrams at most, since the kernel charges
  each its whole buffer, not just its bytes; measured when built); every drop counted from the kernel's
  own count (`SO_RXQ_OVFL`, a count with each datagram); and the highest pace sustained without
  kernel drops measured for each setting. *Option for Shep:* raising `rmem_max` in bench mode
  (a sysctl in `bench-on`, restored by `bench-off`, so in lockfree-ipc's `tools/machine`), only
  if the 4 MB buffer is shown to drop at the day's pace.
- **D4. One thread or two.** *Proposed, both built and measured:* (a) one pinned thread
  receiving, sequencing, decoding and handling (note 001's principle: one owner of the state);
  (b) a receiving thread (socket and sequencer) handing messages to a handling thread through
  `lockfree-ipc`'s SPSC ring, so the socket is drained while a long handling step runs. The
  default chosen from the figures.
  *Chosen (agreed with Shep, option A):* (a), one thread: the day's busiest millisecond is 35,000
  packets a second (16,102 over its busiest second), a third of one thread's keeps-up pace, and at
  such paces one thread is the quickest on every path; (b) wins only near 100k with kernel busy
  polling, for a second CPU. On the card, kernel busy polling (`net.core.busy_read`) is the
  recommended setting: p99 at 50k 3.4–3.7 µs against 13.8 µs (interrupt bursts).
- **D5. Recorded input, through the same path.** *Proposed:* a recording is the datagrams as they
  were received (note 009 writes them), replayed into the sequencer as if from the socket, so
  everything from the sequencer on is the live code. The sample day, which is messages, not
  datagrams, is packed by note 005's packetizer first, as the sender does.
- **D6. The sender.** *Proposed:* its own process, on its own reserved CPU, replaying the day
  from a session store onto the group: at a fixed number of packets a second, or at the day's own
  pace (its timestamps), sped up by a chosen factor; heartbeats when it has nothing to send and
  the end of session after the last, as the specification has it. The same process answers
  re-requests on a unicast port from the same store. A loss setting drops a chosen share of
  packets before sending, so recovery is exercised over a real socket.
- **D7. Paths.** *Proposed:* loopback, and the card: the sender on the far port in the namespace,
  the receiver on ours, multicast routed over the cable. `card-link` is copied into this
  repository's `tools/` (it is the gateway's, written here) with a multicast route added.
- **D8. Receive timestamps.** *Proposed:* each datagram's kernel receive time (`SO_TIMESTAMPNS`)
  read with it, so the time from the kernel to the message being handled can be measured
  (note 010 measures the whole path, from the sender). The card stamps only PTP packets in
  hardware (the gateway's note 005), so these are the kernel's software stamps.
- **D9. Correctness.** *Proposed:* the sample day sent over loopback and over the card, at
  several paces and loss settings: every message handled once, in order, the books equal to
  the reference's at the end; kernel drops counted and recovered by re-request; a recording
  replayed gives the same books (note 001, S7, in part).
- **D10. Measurement.** *Proposed:* the receive path's cost per datagram (D1's two ways, D4's two
  ways), the highest pace sustained without kernel drops for the buffer setting, and the time
  from the kernel's receive stamp to a message handled, as percentiles, open loop, on loopback
  and on the card.

## Spec

- S1. Over loopback and over the card, at every pace and loss setting tested, the sample day's
  messages are handled once, in order, and the books equal the reference's at the end. *Qualified
  (agreed with Shep, option A):* for D4's one thread, the default; two threads at 100k packets a
  second lost the feed in 2 of 4 whole-day runs (see Review), a limit stated, not fixed.
- S2. Every datagram dropped by the kernel is counted (`SO_RXQ_OVFL`) and its messages recovered
  by re-request.
- S3. Every boundary is checked: a datagram whose MoldUDP64 blocks do not fit, a truncated
  datagram, a SoupBinTCP packet longer than what arrived; each counted, never applied.
- S4. A recording of received datagrams, replayed, gives the same books as the live run.
- S5. The receive path makes no allocation; its system calls are the socket's own (`recvmmsg`,
  `sendto` for requests), checked in the compiled code.
- S6. Receive cost per datagram, the highest pace without kernel drops, and kernel-to-handled
  latency are published, loopback and card, for D1's and D4's alternatives, as ranges over at
  least three runs, via `run.py report`. *Amended (agreed with Shep, option A):* also the highest pace
  at which the receiver keeps up (kernel-to-handled p99 under 100 µs), since without kernel drops
  near the limit the buffer only holds a growing backlog.

## Tasks

- [x] The receiver (D1–D3, D8) and the sender with its re-request server (D6)
- [x] D4's two arrangements; D5's replay of recorded datagrams
- [x] `card-link` with a multicast route (D7)
- [x] The sample day over loopback and the card (S1–S4); S5's call-graph check
- [x] The benchmark and the report (S6)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-26 | Draft | Written for review. On this machine today: `net.core.rmem_max` 4 MB (a receive buffer's ceiling without a sysctl), `busy_read` and `busy_poll` 0 |
| 2026-10-26 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 `recvmmsg` batches into padded buffers, `recvmsg` beside it; D2 busy-polling, `SO_BUSY_POLL` measured on the card; D3 the largest buffer allowed, drops counted, `rmem_max` raised only if shown needed; D4 one thread and two, measured; D5 recordings as received datagrams through the same path; D6 our sender with the re-request server; D7 loopback and the card; D8 kernel receive stamps; D9 the day over both paths; D10 cost, drop-free pace and latency |
| 2026-10-26 | Agreed (building; amendment) | **Found by the feed test under TSan, D1 amended.** With the answers on the multicast socket (D1 as agreed, one socket for both), a receiver slower than the sender gave up: the kernel queues a socket's datagrams in arrival order, so each answer waited behind the multicast backlog, while the sequencer's retries (note 005, D2: 5, 1 ms apart) ran out; `lost` at 1,297 of 80,000 messages, all 6 requests answered. Reproduced without TSan (a receiver reading one datagram a call 50 µs apart, ~20k/s against the sender's 50k/s, 2% lost); passed with 50 ms retries, so timing. On the real feed, any lag past ~5 ms (the day's peaks) would fall back to a snapshot. Options put to Shep: A a request socket for requests and answers, read first while gapped; B one socket, retry time counted only once it is drained (recovery still waits for the backlog); C longer retries (moves the threshold). **A agreed.** `feed_test` keeps the slow receiver as a regression case: it fails with requests sent from the multicast socket (0 answers ahead of the backlog, `lost`) and passes with the request socket (43 of 43 answered, ended, the books the reference's). The two sockets' kernel drop counts are kept apart (`kernel_drops`, `answer_drops`) |
| 2026-10-26 | Agreed (building) | **Found by CI, fixed: the sender's end of session.** In CI (a smaller receive buffer, as `net.core.rmem_max` there is presumably the kernel default; a slower machine) several cases stopped `stale`: the receiver's buffer overflowed at the end of the session, taking the last packets and all five end markers (sent a millisecond apart, then nothing), and the kernel reports drops only with a later datagram, so nothing told the receiver. The specification sends End of Session "for a short while in place of Heartbeats", re-requests still allowed meanwhile; D6 said "as the specification has it", so the sender now sends the five, then one a heartbeat until stopped. Not reproduced here (this machine drains the backlog in time even under TSan); CI is the check. The test prints the buffer granted |
| 2026-10-27 | Agreed (building) | **Built since.** `tools/card-link`: the gateway's, with the feed's groups (239.192.0.0/16) routed over the cable on both sides, namespace `itchfh-peer` (not yet run: needs sudo). S5 (`check_hot_path`): `feed_one::poll`, `feed_two::receive_pass` and `handle_one` (the two-thread loop split into these so each side can be checked) call only memory functions, `recvmmsg`, `recvmsg`, `sendto`, `clock_gettime` (the vDSO) and decoder B's table; the socket calls confirmed present in the compiled code. `bench/feed_sender` (D6's own process, driven over a control port: run, stop with its counts, quit) and `bench/feed_bench` (D10: the first 50 million messages at 50k–400k packets a second into each arrangement: kernel drops, the pace achieved from the sender's own send times, cost per datagram, kernel stamp to the first message handled; a case with kernel drops is overloaded, recorded, not a failure); `run.py`'s feed section and README block. A trial outside bench mode (not published): one thread ~8 µs a datagram (about 43 messages through the handler), saturating near 125k packets a second; kernel to handled p50 ~3 µs. **The day over loopback** (`feed_day_test`, label `data`): with requests one at a time (note 005's D2 as built) the 1% cases were lost after bursts of kernel drops (15,000–53,000 over the day with unpinned threads, a quiet machine), as the hold filled faster than one request a round trip refilled it; note 005's D2 amended (agreed, option A: requests in flight together). With the window, 100k packets a second, all pass: one thread with 0.1% dropped by the sender (recorded), one thread and two threads with 1%: 423,285,709 messages handled once, the feed ended, nothing held, the book equal to one fed directly; 18,620–33,427 kernel drops recovered; and S4, the 9,397,004 datagrams recorded as received replayed into a fresh sequencer: the same book. A test flaw on the way: the client (the default book's tables faulted in, seconds) was built after the sender started, so the buffer overflowed at once; now built first |
| 2026-10-27 | Agreed (check run) | **First bench-mode run, loopback** (`a186b7f`, clean; not published, its pace grid since changed; kept outside the repo). Every sustained case handled all 50 million messages. No kernel drops up to 100k packets a second in all three arrangements; drops from 125k. Cost a datagram p50: one thread 8.5 µs (`recvmmsg`), 8.1 (`recvmsg`); two threads' receiving side 2.9. One thread's cost matches the earlier notes' (sequencer 0.43 µs, then 43 messages each decoded, 26 ns, and handled, 151 ns: ~8 µs), so the handler sets the ceiling (~117k a second); two threads do not raise it, the handling thread having the same work, so the ring fills and the socket drops at the same pace. `recvmsg` slightly cheaper and quicker than `recvmmsg` at these paces (datagrams arrive about one at a time, so a batch call mostly returns one). Kernel to handled at 50k: p50 2.5–2.9 µs, p99.9 3.3–3.8 µs; at 100k (86% busy for one thread) p99.9 150–380 µs, queueing. **Agreed with Shep (option A):** the grid made finer where the limit lies (50, 75, 100, 110, 120, 130, 150k; nothing above, where every arrangement is overloaded and the sender is busy with re-requests), then three runs |
| 2026-10-27 | Built (runs, loopback) | **Three bench-mode runs, loopback** (`ee4adfe`, clean; `results/feed-dev-lnx01-20261027T004056Z.json`, `…004554Z`, `…005053Z`), close together. No kernel drops up to: one thread `recvmmsg` 120k packets a second, `recvmsg` 110k, two threads 110k (120k in two runs of three). Batching wins at the limit (near saturation a `recvmmsg` call finds several datagrams; cost falls to 8.0 µs) and loses below it (`recvmsg` 8.1–8.3 µs against 8.5–8.8, kernel to handled p50 2.5 against 2.8 µs). Kernel to handled at 50k: p99.9 3.4–4.0 µs in every arrangement. **S6 amended (option A):** at 110–120k, without drops, p99 was 0.2–9 ms: the receiver behind, the 8 MB buffer (as the kernel reports 4 MB asked) holding the backlog for the ~10 s a case lasts; so the summary adds the highest pace at which it keeps up (p99 under 100 µs): 100k in all three. Published (the loopback part of the set; the card's to follow) |
| 2026-10-27 | Built (runs published) | **The card, with and without kernel busy polling**, three bench-mode runs each (`ee4adfe`, clean; card `…011546Z`, `…012737Z`, `…013236Z`; busy polling, `net.core.busy_read` and `busy_poll` 50 set system-wide for the run and restored to 0, `…013732Z`, then after a reboot of the machine `…113615Z`, `…114103Z`: within ~5% of the first, so one set). Over the card without busy polling, kernel to handled p50 at 50k is 3.3–3.8 µs and p99 13–15 µs (loopback 2.5–2.9 and 3.0–3.5), and p50 grows with the pace (12 µs at 100k against loopback's 3): the card interrupts after up to 24 µs (`rx-usecs`, the gateway's note 005), the kernel stamps a burst together, and the receiver works through it at ~8.4 µs a datagram. Busy polling removes the bursts (p99 at 50k 3.4–5.0 µs) but, our receive calls being non-blocking, each call polls the card's queue once itself, so the driver's receive work moves from the interrupt (a housekeeping CPU) to the receiving thread: one thread +2.3–2.9 µs a datagram, two threads' receiving side 3.1 → 5.3 µs; one thread then falls behind sooner (keeps up to 75k, against 100k without), two threads keep up to 100k with p99 31–32 µs at 100k and p99.9 5.3–5.6 µs at 50k, the best of the card's setups. No kernel drops up to 110k in every arrangement on the card (120k in some runs). **Published** with the loopback runs, nine in all, `results/published.toml`; RESULTS gives every pace per path, the README the summary. Against the Spec: S1, S2, S4 (`feed_day_test`, loopback; the card's day not run, see below), S3 (`feed_test`), S5 (`check_hot_path`), S6 (these runs, amended). Open for Shep: D4's default (the figures: one thread on loopback and the card without busy polling, two threads with it); the card's sample day (D9 says loopback and the card; only the benchmark's 50 million messages crossed the card) |
| 2026-10-27 | Built (defaults) | **D1 and D4 chosen, agreed with Shep (option A each):** one thread, `recvmsg`, with kernel busy polling on the card. The sample day's own rates, packed as the sender packs them (9,358,889 packets, as sent): busiest 1 s 16,102 packets a second (the close), 100 ms 25,250, 10 ms 28,200, 1 ms 35,000; so capacity does not decide, latency at such paces does. At 50k kernel to handled p50 / p99 / p99.9: loopback `recvmsg` 2.5 / 3.1 / 3.4 µs against `recvmmsg` 2.7–2.8 / 3.3–3.5 / 3.7 and two threads 2.9 / 3.6 / 3.9–4.0; the card with busy polling 2.1–2.3 / 3.4–3.7 / 4.0–4.3 against 2.5–2.6 / 4.4–4.8 / 5.2–5.6 and 2.6–2.8 / 4.6–5.0 / 5.3–5.6. Caveat: our sender fills every packet; a real feed sends more, smaller packets as messages occur. `feed_one`'s default is now `recvmsg` (`batch` false). **And the card's day (S1), agreed (option A):** `feed_day_test` gains a mode driving `feed_sender` (in the namespace) over its control port |
| 2026-10-27 | Built (card day, prepared) | `feed_day_test` drives `feed_sender` over its control port when given `card <our ip> <sender ip>`, and is now placed as `feed_bench` is (receiving on CPU 2, handling on 3, its own sender on 4). Tried first over loopback against `feed_sender`: unpinned, every case was lost (the sender pinned to CPU 4 and late for 114,508 of its first 652,294 packets in the first; 1.24M kernel drops in the second, at the edge of `recvmsg`'s capacity for 94 s); pinned, the one-thread cases pass (the new default, `recvmsg`), 27,080–64,925 kernel drops recovered with no retry and no full hold, and S4's replay; two threads were lost once in two runs (34,341 kernel drops, at 17.6 million messages) and passed in the other (26,391 recovered). **Open:** two threads lost once at 100k packets a second over the day; a guess, not shown: when the handling thread falls behind the ring fills, the receiving thread waits on it instead of reading the socket, which then overflows, and the recovery adds to the handling thread's work; one thread never waits on another |
| 2026-10-27 | Built (card day) | **The day over the card** (`feed_day_test … card 10.99.0.1 10.99.0.2`, `feed_sender` in the namespace, 100k packets a second): one thread, 0.1% dropped by the sender and recorded, and 1%: 423,285,709 messages handled once, ended, nothing held, the book equal to one fed directly, 28,032 and 64,490 kernel drops recovered; S4, the 9,391,368 datagrams recorded across the card replayed: the same book. Two threads, 1%: lost at 17,508,013 messages, the hold full (8,798,138 packets refused). That was the second such loss, both at the same point of the day (the earlier, over loopback, at 17,568,841): 09:31:06–09:31:12, ordinary adds, deletes and executions just after the open. Instrumented once (not committed): the receiving thread waited on a full ring 2.1 million times over the day, 538 ms in all, its longest pass 12.6 ms and 220 passes over 1 ms; during such a pass the request socket is not read, so recovery stalls while the hold fills (that run passed). **Agreed with Shep (option A):** recorded as found, two threads kept as the measured alternative with this limit, S1 qualified; the remedy, if wanted later, is a sequencer that delivers a backlog in bounded chunks so the receiving side returns to its sockets between them (note 005). Every task done; for Shep's review for Measured, with the sequencer's figures to be confirmed by a check run (note 005's D2 changed) |

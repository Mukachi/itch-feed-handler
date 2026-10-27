# 009 — Recording and monitoring

**Status:** Draft · **Updated:** 2026-10-27

## Proposal

Components 11 and 12 of note 001, kept minimal as it agreed. The **data recorder** saves what was
received, for investigation, testing and replay, without slowing the main path, and says whether
the capture is complete. **Configuration and monitoring** define the feed, the limits and the
CPUs in one place, and report the handler's health: connection, gaps, delays and failures.

Earlier notes fix part of it. Note 007's D5: a recording is the datagrams as they were received,
replayed into the sequencer as if from the socket, so everything from the sequencer on is the live
code. Note 001's S7: a run received over multicast and recorded, replayed from the recording,
gives the same books and the same published updates.

The handler has so far been assembled only inside tests and benchmarks. This note also adds the
program that runs it: receiving (note 007), sequencing (005), decoding and handling (004, 003),
publishing (008), recording and monitoring, from one configuration file. It does not add
alerting, dashboards or a metrics service: health is a block of shared memory and a tool that
reads it.

## Design

```mermaid
flowchart LR
  S[sockets] --> F[feed loop, one thread]
  F -->|datagram copy| RR[ring] --> W[writer thread] --> D[(recording file)]
  F --> P[publisher, note 008]
  F -->|counters, every 100 ms| M[monitoring block]
  M --> T[itchfh-monitor]
  D --> RP[replay] --> F2[sequencer onwards, as live]
```

**Decisions** (each with what is proposed):

- **D1. What is recorded.** *Proposed:* every datagram as the receive thread read it, from both
  sockets (the group and the request socket, note 007's D1), in the order read, each with its
  length, the socket, and the kernel's receive stamp (note 007's D8). *Alternative:* the decoded
  messages, which lose what the network did (loss, order, answers) and so cannot test recovery.
- **D2. Not slowing the main path.** *Proposed:* the receive thread copies each datagram into
  an SPSC ring (`lockfree-ipc`, slots of 1,600 bytes, 16,384 of them: about half a second at the
  day's busiest millisecond); a writer thread on a housekeeping CPU empties it to the file. The
  receive thread never waits: a full ring means the datagram is not recorded, counted, and the
  recording marked incomplete from there. *Alternative:* writing from the receive thread, which
  puts every system call and every disk stall on the path.
- **D3. Writing the file.** *Proposed:* large sequential writes (4 MB buffers), with
  `sync_file_range` started on each written buffer so dirty pages never build up to the kernel's
  30-second writeback (the gateway's note 004: ext4 writeback stalled its engine 0.3–2.5 ms every
  5 s once a file had been dirty that long), and `fdatasync` at the end. The disk here: ext4 on the
  P440ar's one SAS disk, 827 GB free; the sample day is 12.5 GB of datagrams, 22 MB a second in
  its busiest second. *Alternative, measured only if the first disturbs the receive thread:*
  `O_DIRECT`.
- **D4. Complete or not.** *Proposed:* a header (magic, version, session, the configuration's
  digest, start time) and a trailer (datagrams recorded, datagrams not recorded, the kernel's
  drop counts at the end, end time). A recording is complete when its trailer is there, nothing
  was left unrecorded, and its replay ends with no gap left open. A recording cut short (the
  process killed) has no trailer: it is read up to its last whole record and reported incomplete.
- **D5. Replay.** *Proposed:* a recording read datagram by datagram into the sequencer, as note
  007's D5 has it, then decoder, handler and publisher, at full speed or at the recorded pace (the
  kernel stamps); the program's `--replay <file>` in place of the sockets.
- **D6. Monitoring.** *Proposed:* every component's counters (receive, sequencer, decoder,
  handler, book, publisher, recorder) and a health state for each (feed: its sequencer state,
  seconds since the last datagram; recorder: backlog, unrecorded; publisher: consumers and
  overruns) copied by the feed loop's tick every 100 ms into a monitoring block in shared memory,
  under a seqlock (note 008's), with no allocation or system call. A tool, `itchfh-monitor`, prints
  it once or every second, and exits non-zero if anything is unhealthy, for scripts.
  *Alternative:* logs or a metrics endpoint, more than "kept minimal" calls for.
- **D7. Configuration and the program.** *Proposed:* one file of `key = value` lines (the
  group, interface, ports and re-request server; receive buffer and busy polling; CPUs; book,
  hold and ring sizes; the publisher's name; the recording's directory), every key validated at
  start, unknown keys refused; its digest kept in the recording's header. The program `itchfh`
  runs one feed from it until the end of session or a signal. *Alternative:* command-line flags
  only, which a recording could not cite.
- **D8. Measurement.** *Proposed:* the recorder's cost on the receive thread a datagram (the
  copy into the ring); kernel to handled (note 007's measure) with recording on and off, to show
  it does not disturb the path; the writer's sustained rate; at the paces of note 007's benchmark.
  End to end, from the packet to the published update, is note 010's.

## Spec

- S1. The sample day received over loopback at 100k packets a second, with packets dropped by
  the sender, recorded, then replayed from the file, gives the same books and the same published
  updates (a digest of the update stream) as the live run (note 001's S7), and the recording is
  reported complete.
- S2. The receive path never waits on the recorder: with the writer held back on purpose, the
  feed carries on, the datagrams not recorded are counted, and the recording is reported
  incomplete from the first of them.
- S3. A recording cut short (the program killed) is read up to its last whole record and reported
  incomplete; a corrupted record is detected, not replayed.
- S4. The monitoring block is never read torn; at the end its counters equal the components'
  own; each health state changes as forced (silence, a loss, a consumer overrun, the recorder held
  back).
- S5. The recorder's path on the receive thread and the monitoring refresh make no allocation or
  system call (checked in the compiled code).
- S6. The recorder's cost a datagram, kernel to handled with recording on and off, and the
  writer's sustained rate are published as ranges over at least three runs, via `run.py report`.

## Tasks

- [ ] The recording format, the ring and writer (D1–D4)
- [ ] Replay from a file (D5)
- [ ] The monitoring block and `itchfh-monitor` (D6)
- [ ] The configuration and the program `itchfh` (D7)
- [ ] Tests (S1–S4); the compiled-code check (S5)
- [ ] The benchmark and the report (S6)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-27 | Draft | Written for review. On this machine today: `/` is ext4 on `sda`, the P440ar's logical volume (one SAS disk, its cache battery-backed: the gateway's note 004), 827 GB free; `vm.dirty_expire_centisecs` 3000 (30 s), `dirty_background_ratio` 10, `dirty_ratio` 20. The sample day's datagrams are 12.5 GB (note 007's recordings: 9.39 million), at 0.22 MB a second on average and about 22 MB a second in its busiest second (16,102 packets) |

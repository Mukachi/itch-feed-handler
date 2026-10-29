# itch-feed-handler

A Nasdaq TotalView-ITCH 5.0 feed handler in C++20 for Linux, written from the public
specifications: it receives the feed over MoldUDP64 multicast, recovers lost packets, rebuilds
every order book, and publishes the books to other processes, each update marked as current or
not. Every component is checked against a simple reference implementation over a whole real
trading day (Nasdaq's sample for 30 January 2020), and measured open loop on the harness of
[lockfree-ipc](https://github.com/Mukachi/lockfree-ipc), beside that reference. Where something
is unexplained, the notes say so.

## Results

Ranges over at least three runs on a dedicated, isolated machine (conditions below). Full tables,
the method and every run's conditions: [RESULTS.md](RESULTS.md). These blocks are generated from
the committed JSON in `results/` by `python run.py report`; the test suite fails if they disagree.

End to end: a packet's intended send time to a consumer process reading the update it causes,
and the whole day's throughput ([note 010](docs/design/010-measurement.md)):

<!-- results:e2e-summary -->
| Path | Case | Packet to published update p50 | p99 | p99.9 |
|---|---|---|---|---|
| loopback | 25k/s | 11.3 µs–11.7 µs | 21.4 µs–22.0 µs | 26.7 µs–27.3 µs |
| loopback | the open, its own pace | 11.7 µs–12.4 µs | 23.2 µs–24.3 µs | 34.7 µs–36.7 µs |
| the card, kernel busy polling | 25k/s | 24.1 µs–24.5 µs | 34.2 µs–35.1 µs | 38.1 µs–39.0 µs |
| the card, kernel busy polling | the open, its own pace | 25.1 µs–25.4 µs | 38.2 µs–39.2 µs | 52.4 µs–54.5 µs |

| Case | M messages/s | Seconds for the day |
|---|---|---|
| fast | 4.23 | 100.0–100.2 |
| fast, published to one consumer | 2.57–2.62 | 161.3–164.6 |
| reference | 1.01–1.02 | 413.9–417.7 |

One way, open loop, from the packet's intended send time to a consumer process reading the update, recording on; and the whole day from a recording at full speed. ns, range over runs. The method and every pace: [RESULTS.md](RESULTS.md).
<!-- /results -->

Recovering from a snapshot when the feed is lost ([note 011](docs/design/011-snapshot-recovery.md)):

<!-- results:recovery-summary -->
Recovery figures: not yet measured (design note 011).
<!-- /results -->

Receiving, over loopback and a 10GBASE-T card ([note 007](docs/design/007-receiver-framer.md)):

<!-- results:feed-summary -->
| Path | Arrangement | Keeps up (p99 < 100 µs) | No kernel drops | Cost p50 at 100k/s | Kernel to handled p50 at 100k/s | p99 |
|---|---|---|---|---|---|---|
| loopback | one, recvmmsg | 100k/s | 120k/s | 8,606–8,766 | 2,871–3,003 | 19.6 µs–27.1 µs |
| loopback | one, recvmsg | 100k/s | 110k/s | 8,140–8,271 | 2,495–2,555 | 35.8 µs–69.5 µs |
| loopback | two | 100k/s | 110k/s | 3,021–3,076 | 2,979–3,027 | 22.2 µs–49.9 µs |
| the card | one, recvmmsg | 100k/s | 110k/s | 8,388–8,402 | 12.2 µs–12.3 µs | 48.1 µs–48.8 µs |
| the card | one, recvmsg | 75k/s | 110k/s | 8,417–8,460 | 12.4 µs–12.7 µs | 169.5 µs–204.3 µs |
| the card | two | 100k/s | 110k/s | 2,249–2,395 | 13.1 µs–13.5 µs | 56.7 µs–62.4 µs |
| the card, kernel busy polling | one, recvmmsg | 75k/s | 110k/s | 9,596–9,625 | 22.9 µs–23.6 µs | 123.5 µs–137.7 µs |
| the card, kernel busy polling | one, recvmsg | 75k/s | 110k/s | 8,708–8,737 | 11.1 µs–11.2 µs | 126.6 µs–189.7 µs |
| the card, kernel busy polling | two | 100k/s | 110k/s | 5,206–5,337 | 2,711–2,787 | 30.6 µs–32.3 µs |

The first 50,000,000 messages of the sample day over MoldUDP64 multicast from our sender, received, sequenced, decoded and handled on the default book, in packets a second (about 43 messages each); ns, range over runs; cost a datagram, for two threads the receiving side's alone. No kernel drops: over about ten seconds, the socket's buffer holding any backlog. Every pace and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

Publishing to consumer processes ([note 008](docs/design/008-publication.md)) and recording
what is received ([note 009](docs/design/009-recording-monitoring.md)):

<!-- results:publish-summary -->
| Case | M messages/s | p50 | p99 | p99.9 | Write to read p50, first consumer | p99 | Write to read p50, last consumer |
|---|---|---|---|---|---|---|---|
| no publisher | 4.31–4.32 | 197 | 628–629 | 892–894 | – | – | – |
| table only | 3.08–3.10 | 269–270 | 764–768 | 1,055–1,061 | – | – | – |
| 1 consumer | 2.79–3.34 | 229–291 | 699–811 | 997–1,114 | 281–316 | 556–670 | – |
| 2 consumers | 2.35–2.43 | 350–364 | 894–913 | 1,194–1,212 | 322–377 | 686–741 | 349–391 |
| 4 consumers | 1.77–1.90 | 453–489 | 1,086–1,145 | 1,394–1,456 | 340–534 | 756–959 | 372–471 |

The sample day's first 50,000,000 messages through the handler and its publisher on one CPU, consumers in processes of their own; ns, range over runs. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:record-summary -->
| Pace | Recording | Cost a datagram p50 | p99 | Kernel to handled p50 | p99 | p99.9 | Kernel drops |
|---|---|---|---|---|---|---|---|
| 50k/s | off | 8,169–8,256 | 11.2 µs–11.3 µs | 2,459–2,515 | 3,039–3,107 | 3,303–3,395 | 0 |
| 50k/s | on | 9,014–9,057 | 12.7 µs–12.8 µs | 2,887–2,923 | 3,599–3,643 | 3,931–3,999 | 0 |
| 100k/s | off | 8,169–8,271 | 11.2 µs–11.3 µs | 2,515–2,591 | 39.4 µs–79.6 µs | 383.5 µs–790.5 µs | 0 |
| 100k/s | on | 8,999–9,057 | 12.6 µs–12.8 µs | 3,471–4,167 | 1,245.2 µs–2,457.6 µs | 3,006.5 µs–4,587.5 µs | 0 |

The receive path with the recorder off and on: one thread over loopback, ns, range over runs. The writer flat out and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

The components alone, in process: the order book ([note 002](docs/design/002-order-book.md)),
the handler ([note 003](docs/design/003-events-directory-status.md)), the decoder
([note 004](docs/design/004-decoder.md)), sequencing ([note 005](docs/design/005-sequencing-recovery.md))
and snapshots ([note 006](docs/design/006-snapshots-replay.md)):

<!-- results:book-summary -->
| Book | M events/s | p50 | p99 | p99.9 | max | Peak MB |
|---|---|---|---|---|---|---|
| reference book | 1.83 | 583–587 | 1,561–1,572 | 2,206–2,220 | 171,010.1 µs–171,524.7 µs | 231 |
| refs A: unordered_map, levels A: std::map | 2.96–2.97 | 318–319 | 1,130–1,132 | 1,620–1,621 | 1,353.3 µs–1,366.6 µs | 299 |
| refs A: unordered_map, levels B: sorted array | 3.91–3.93 | 260 | 894–896 | 1,246–1,250 | 663.6 µs–668.2 µs | 387 |
| refs A: unordered_map, levels C: price window | 4.11–4.12 | 254–260 | 872–877 | 1,226–1,234 | 659.8 µs–663.0 µs | 653 |
| refs B: open addressing, levels A: std::map | 4.23–4.26 | 208–209 | 803–809 | 1,228–1,239 | 402.6 µs–409.0 µs | 337 |
| refs B: open addressing, levels B: sorted array | 6.33–6.35 | 174 | 561 | 820–821 | 17.6 µs–41.4 µs | 425 |
| refs B: open addressing, levels C: price window | 6.64–6.67 | 168 | 546–548 | 808–811 | 16.9 µs–17.7 µs | 691 |
| refs C: direct array, levels A: std::map | 5.24–5.28 | 167 | 702–709 | 1,097–1,103 | 393.0 µs–398.1 µs | 2,385 |
| refs C: direct array, levels B: sorted array | 8.94–8.98 | 138 | 466–467 | 731–732 | 17.0 µs–18.0 µs | 2,473 |
| refs C: direct array, levels C: price window | 9.54–9.62 | 136–137 | 458–461 | 721–723 | 17.2 µs–17.4 µs | 2,739 |

The sample day's 417,228,155 events, one CPU, ns per event, range over 3 runs. By event type, the method and the conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:handler-summary -->
| Setup | M messages/s | p50 | p99 | p99.9 | max | Peak MB |
|---|---|---|---|---|---|---|
| reference handler, reference book | 1.19–1.20 | 816–820 | 2,038–2,046 | 2,839–2,854 | 516,292.9 µs–516,465.9 µs | 1,052 |
| handler, default book | 7.15–7.23 | 151–152 | 543–558 | 801–817 | 17.7 µs–21.4 µs | 3,274 |

Every message of the sample day (423,285,709), already decoded, one CPU, ns per message, range over 3 runs. By message type and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:decoder-summary -->
| Decoder | M messages/s | p50 | p99 | p99.9 | Mispred./msg | With handler M/s | With handler p50 | With handler p99 |
|---|---|---|---|---|---|---|---|---|
| reference | 61.1–68.5 | 30–32 | 40–43 | 121–125 | 0.558–0.559 | 5.97–6.31 | 157–177 | 516–584 |
| A: switch | 72.3–72.4 | 29 | 36–37 | 116–126 | 0.551 | 6.68 | 156–157 | 517–518 |
| B: table | 82.0–82.1 | 26 | 35 | 115–116 | 0.552 | 6.74–6.75 | 154 | 518–519 |
| C: SIMD, padded | 41.9–43.6 | 43 | 52–53 | 56 | 0.484 | 6.29–6.31 | 167 | 519–521 |
| C: SIMD, unpadded | 35.3–35.9 | 48 | 59 | 123–144 | 0.492–0.493 | 5.56–5.57 | 186–187 | 592–593 |

Every message of the sample day (423,285,709) in its order, one CPU, ns per message, range over 3 runs; with handler: decoded and handled on the default book. By message type and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:sequencer-summary -->
| Measure | Rate | p50 | p99 | p99.9 |
|---|---|---|---|---|
| in order, a packet | 2.30 M packets/s, 104.1 M messages/s | 428–429 | 810–811 | 869–873 |
| held after a gap, a packet | – | 296–374 | 820–883 | 1,024–1,243 |
| the gap filled, 10 held delivered | 217–232 ns a packet | 2,359–2,555 | – | – |
| the gap filled, 100 held delivered | 221–292 ns a packet | 22.4 µs–29.5 µs | – | – |
| the gap filled, 1,000 held delivered | 213–265 ns a packet | 214.1 µs–265.6 µs | – | – |
| the gap filled, 10,000 held delivered | 222–270 ns a packet | 2,218.2 µs–2,702.8 µs | – | – |

The sample day in 9,358,889 MoldUDP64 packets, one CPU, ns, range over 6 runs; the sequencer's own work. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:snapshot-summary -->
| At message | Live orders | Spin messages | Spin MB | Build ms | Apply ms |
|---|---|---|---|---|---|
| 412,349,206 (the peak) | 1,925,638 | 1,952,366 | 79 | 1,211–1,215 | 362–369 |

A GLIMPSE 5.0 snapshot at the sample day's peak of live orders: built by the snapshot server, applied by a client into a fresh book, one CPU, range over 3 runs. Every point of the day: [RESULTS.md](RESULTS.md).
<!-- /results -->

## What is here

| Component | Note | Header |
|---|---|---|
| Order book: orders by reference, price levels per side | [002](docs/design/002-order-book.md) | `book.hpp` |
| Event handler: instrument directory, trades, market status, imbalances | [003](docs/design/003-events-directory-status.md) | `handler.hpp` |
| ITCH 5.0 decoder | [004](docs/design/004-decoder.md) | `decoder.hpp` |
| MoldUDP64 sequencer: gaps held, exact re-requests, retries | [005](docs/design/005-sequencing-recovery.md) | `sequencer.hpp` |
| GLIMPSE 5.0 snapshots over SoupBinTCP, server and client | [006](docs/design/006-snapshots-replay.md) | `snapshot.hpp`, `soup.hpp` |
| Multicast receiver and framer | [007](docs/design/007-receiver-framer.md) | `udp.hpp`, `feed.hpp` |
| Publisher: a ring per consumer process, a seqlocked state table | [008](docs/design/008-publication.md) | `publisher.hpp`, `consumer.hpp` |
| Recorder and replay; monitoring block | [009](docs/design/009-recording-monitoring.md) | `recording.hpp`, `replay.hpp`, `monitoring.hpp` |
| Reference book, handler and decoder: the oracles and baselines | 002–004 | `reference_*.hpp` |

Header-only, in `include/itchfh/`. The program `itchfh` (`apps/`) runs them as one thread
(receive, sequence, decode, handle, publish) with a writer thread for the recording; it reads a
`key = value` configuration (`config.hpp`), and `itchfh-monitor` reads its monitoring block. A
consumer is its own process: [examples/publish_consumer.cpp](examples/publish_consumer.cpp).
For testing and measurement, MoldUDP64 sender and re-request server of our own
(`mold_sender.hpp`, `session_store.hpp`), since Nasdaq's feed is not available here.

In steady state the hot paths allocate nothing, and a test checks their compiled call graphs:
no allocation, and no system call but the socket reads and writes each is meant to make.

## Correctness

- **The whole day against the reference.** Every component is run over the sample day's 423
  million messages and compared with the reference after every one: the fast book against the
  reference book, the handler against the reference handler, decoder against decoder.
- **Loss and recovery.** The day through a lossy link: every message handled exactly once, in
  order, with packets dropped at random by the sender and by the kernel, over loopback and over
  the card. Snapshots: the encoder reproduces the day byte for byte, and a book rebuilt from a
  snapshot at any point and fed the rest of the day equals the reference.
- **The program.** The day received and recorded by `itchfh`, then replayed from the recording
  to two consumer processes: both consumers' books equal a book fed the day directly, and every
  update is the same byte for byte as the live run's.
- **Fuzzing.** A libFuzzer target for the decoder, differential against the reference decoder;
  its corpus is committed (`fuzz/`).
- **CI** on every push: x64, ThreadSanitizer, AddressSanitizer/UBSan and arm64. The whole-day
  checks need the sample day and run here, not in CI.

## Findings

- **The measurement found its own fault.** Every end-to-end case had the same millisecond
  maximum, at every pace. It was the benchmarks: they forked their consumer process after
  building the producer, so the producer's pages were copy-on-write and its first write to each
  took a fault. A stretch of the day that writes a new page a message showed it. Consumers are now
  forked first, and every affected set was re-measured (note 010).
- **One thread, not two.** Receiving on one thread and handling on another is slower at these
  paces, and over the whole day it was lost twice at the same point: the receiving thread waited
  on a full ring while its socket overflowed. One thread never waits on another (note 007).
- **The consumer's socket matters.** A consumer on the other NUMA node slows the publisher for
  every consumer; the publisher counts such consumers and reports them, and the example pins
  itself to the publisher's node (notes 008, 009).
- **The data.** Every crossed book of the day began in a halt or a pause (note 002). Match
  numbers repeat, each first in a non-printable execution and then a printable one, so volume
  counted from printable executions alone counts each once (note 003).
- **Open.** At the higher fixed paces a few-millisecond maximum recurs in every run, a guess not
  shown: a stretch of the day costlier than the pace allows (note 010). The sequencer's hold and
  backlog run at two levels in some runs, cause not found (note 005). The recorder costs more in
  the feed than alone (note 009).

## How it is measured

- **Open loop.** The sender paces packets on a fixed schedule and writes each message's intended
  send time to shared memory; latency runs from that time to a consumer's read, so a stall delays
  every packet due during it. The sender's own lateness is recorded beside it.
- **The harness** is lockfree-ipc's, fetched at a pinned commit: TSC clock, percentiles from a
  log-linear histogram after a discarded warm-up, the noise floor and the machine's conditions in
  every run, and a refusal to publish unless the machine is in bench mode and the build is clean.
- **The machine.** HPE ProLiant DL380 Gen9, 2 × Xeon E5-2699 v4, Ubuntu 26.04, kernel 7.0, set
  up as lockfree-ipc describes ([its machine scripts](https://github.com/Mukachi/lockfree-ipc/tree/main/tools/machine)):
  reserved, isolated CPUs on node 0, with services, kernel threads and interrupts fenced off them.
  The card is a Broadcom BCM57810, its two 10GBASE-T ports joined by a cable, the sender's port
  in a network namespace (`tools/card-link`).

## The sample day

Nasdaq publishes sample days of TotalView-ITCH 5.0 at emi.nasdaq.com without stated terms, so
none is in this repository and none is redistributed. To fetch the one used here (5.6 GB, size
and SHA-256 checked) into `data/`, which git ignores:

```sh
python tools/fetch_day.py
```

## Build and test

Linux; C++20; tested with GCC 15.2, CMake 4.2 and Python 3.14.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build -LE data
```

CMake fetches lockfree-ipc at a pinned commit into `build/`. Checks that need the sample day carry
the CTest label `data` (several take an hour or more); checks that need a quiet, dedicated machine
carry `quiet`. `-DITCHFH_FUZZ=ON` (with clang) builds the fuzzer. The CI script,
[ci/test.sh](ci/test.sh), runs the same builds locally.

## Running the benchmarks

```sh
build/book_bench out.json            # and handler_, decoder_, sequencer_, snapshot_, publish_bench
build/throughput_bench out.json      # the whole day from a recording (13.4 GB, made in build/)
build/feed_sender 127.0.0.1 239.192.0.1 26400 26401 4 0 data/01302020.NASDAQ_ITCH50.gz /itchfh-stamps &
build/feed_bench loopback 127.0.0.1 127.0.0.1 out.json      # receiving (note 007)
build/record_bench 127.0.0.1 127.0.0.1 out.json             # recording (note 009)
build/e2e_bench loopback 127.0.0.1 127.0.0.1 out.json       # end to end (note 010)
```

The network benchmarks drive the sender over its control port; for the card, the sender runs in
the namespace on 10.99.0.2 and the benchmark is given `card` (`card-busy-poll` for `e2e_bench`)
and 10.99.0.1. Each writes one JSON file: the environment, the noise floor, the clock, then the
results. Run without a path, a benchmark writes into `results/`, and refuses unless the machine is
in bench mode and the build is clean; given a path elsewhere, it runs anywhere and marks the file
unpublished. The CPU numbers are this machine's; another machine needs them changed in the
benchmark source. To publish runs: list them in `results/published.toml`, then
`python run.py report`.

## Design notes

Every component has a design note written and agreed before its code: proposal, design, spec,
tasks, and a review log with every finding, including the wrong turns. Index:
[docs/README.md](docs/README.md).

## Licence

[MIT](LICENSE). Nasdaq, TotalView, ITCH, MoldUDP64, SoupBinTCP and GLIMPSE are Nasdaq's; this is
an independent implementation of their published specifications.

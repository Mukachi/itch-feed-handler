# itch-feed-handler

A Nasdaq TotalView-ITCH 5.0 feed handler in C++20 for Linux: it receives the exchange's market
data, rebuilds its order books and publishes them, marked as current or not. Work in progress: see
[docs/design/001-scope.md](docs/design/001-scope.md).

## Results

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

<!-- results:publish-summary -->
Publication figures: not yet measured (design note 008).
<!-- /results -->

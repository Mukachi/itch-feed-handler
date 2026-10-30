# Open items

Every item a note's Review leaves open, one row each. Each gets one decision: **fix** (a task;
closed by its commit), **limit** (stated where it qualifies a figure; closed) or **parked**
(changes no published conclusion; closed, with what would reopen it). A note does not reach
Measured while an item it raised has no decision.

Decided with Shep 2026-10-30: every item below as written.

| # | Item | Note | Touches a published figure | Decision |
|---|---|---|---|---|
| I1 | Note 008's publish set not re-measured after the mute change (a branch on every publish) | 011, 008 | Publish set | fix: three `publish_bench` runs, republish |
| I2 | A backlog after recovery is delivered in one call: the day test at 15k packets/s had 2,642 kernel drops; also the remedy for I3 | 011, 007 | No (S3 is at the peak's pace) | limit: stated in note 011 and the README; chunked delivery not built |
| I3 | Two receiving threads lost the feed in 2 of 4 whole-day runs after the open | 007 | Two-thread rows | limit: already S1's stated qualification; one thread is the default |
| I4 | The peak of live orders is quiet (~1.3k packets/s); a recovery at the busiest second (~16k) would hold ~12x as much | 011 | Recovery set | limit: stated beside the recovery figures |
| I5 | Recovery's waiting part is our test service building on demand, not the program | 011 | Recovery set | limit: stated beside the recovery figures |
| I6 | e2e maxima at 25k (5.6–6.0 ms) and 50k (11.2–11.6 ms) every run; a guess, a costly stretch of the day | 010 | e2e maxima | limit: already in the README's Open |
| I7 | `record()` ~200 ns alone, ~800 ns a datagram in the feed (a guess: cache misses) | 009 | Record set | limit: published as measured (agreed 27 Oct) |
| I8 | The sequencer's hold and backlog run at two levels (~20%, one run in six); suspect physical placement | 005 | Sequencer held/backlog rows | limit: published as ranges, already in the README's Open |
| I9 | Publish: the no-publisher baseline moved between builds; the table's ~72 ns; consumers' remaining cost | 008 | Publish set | parked: reopen if the publisher changes |
| I10 | Program's apply 34–46 ms above note 006's client alone (a guess: slices, socket reads, table rebuild) | 011 | Recovery set | parked: reopen if recovery time matters |
| I11 | Reference decoder throughput spread 61–68 M msgs/s (predictor ruled out) | 004 | Reference row | parked: a baseline, not ours |
| I12 | Snapshot apply: the first two points at two levels in two runs of three; a 49 µs noise stop in note 009's runs | 006, 009 | No (the peak point unaffected) | parked: reopen if they recur at a published point |

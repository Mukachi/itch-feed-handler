# 002 — Order book

**Status:** Agreed · **Updated:** 2026-10-25

## Proposal

The books for every instrument on the feed, built order by order from TotalView-ITCH 5.0's Add,
Execute, Cancel, Delete and Replace events: each displayed order, the price levels they make, and
the best bid and offer. It is component 7 of note 001, first in Shep's order because every other
component exists to feed it or to read it.

It also builds the **reference book** (note 001, D3): the same events applied by code written for
clarity, not speed. It is the oracle every test compares against and the baseline every
measurement is set beside.

It does not decode messages (note 004), route them (note 003) or publish (note 008). Its input is
an event, already decoded; its output is the changed state, which those notes take further.

## Design

**What the events say** (TotalView-ITCH 5.0, sections 1.3 to 1.4):

| Event | Carries | Effect on the book |
|---|---|---|
| Add Order (`A`, `F`) | reference number, side, shares, price, instrument (`F`: also the market participant) | a new order at the back of its price level |
| Order Executed (`E`) | reference number, shares executed | the order's shares reduced; removed at zero |
| Order Executed with Price (`C`) | as `E`, and the execution price | as `E`: the display price does not change |
| Order Cancel (`X`) | reference number, shares cancelled | the order's shares reduced |
| Order Delete (`D`) | reference number | the order removed |
| Order Replace (`U`) | old and new reference numbers, new shares and price | the old order removed; a new order, same side and instrument, at the back of its new level |

Every message carries the instrument's Stock Locate code, but only Add carries the side and price.
So every other event first finds its order by reference number, a number the specification calls
day-unique. That lookup, not the price levels, is the step every event pays. Prices are integers
with four implied decimals, at most 200,000.0000; shares are 32-bit.

Trades of non-displayed orders (`P`), cross trades (`Q`) and broken trades (`B`) do not change the
displayed book (the specification says so); they are note 003's.

**The structures:**

```mermaid
flowchart LR
  E[event] --> T[reference number table]
  T --> O[order: side, price, shares, instrument, level, neighbours]
  O --> L[price level: total shares, order count, first and last order]
  L --> B[book per instrument: bid levels, ask levels, best of each]
```

- **Orders** live in one pool of fixed-size records, allocated at start. An order points to its
  level and to its neighbours in that level's queue, so removing one is constant time.
- **Price levels** hold the total displayed shares, the number of orders and the first and last
  order, the queue in time priority. Consumers that care about queue position (note 008) can
  walk it; consumers that want depth read the totals.
- **A book** per instrument, indexed by Stock Locate code (a small integer assigned each day, meant
  as an array index), with each side's levels and its best price.

**Decisions** (each with what is proposed):

- **D1. Finding an order by reference number.** Built three ways and measured on the sample day:
  - *A.* `std::unordered_map` (what the reference book uses);
  - *B.* open addressing over a fixed-capacity table, allocated and pre-faulted at start, as the
    gateway's order book does (its note 007, and the page-fault lesson of its note 008);
  - *C.* a direct array indexed by the reference number less a base. The specification promises
    only uniqueness. On the sample day the numbers run from 8 to 494,466,152 for 223 million new
    orders (Add and Replace), so about 45% of the range is ever used, and they do not arrive in
    increasing order (159 million are below the highest seen before them). An array of 32-bit
    pool indexes over that range is 2.0 GB, nearly all of it touched over the day, against at
    most 1.93 million orders live at once: memory and TLB misses against one hashing step. A
    number outside the range must fall back to B.
  *Proposed:* all three built and measured; the default chosen from the figures.
- **D2. Keeping price levels per side.** Built and measured:
  - *A.* `std::map` from price to level (the reference book);
  - *B.* a sorted array of levels, best at the end, since most changes happen near the best;
  - *C.* an array indexed by price in ticks around the best, recentred when the best moves far.
    Ticks are not uniform (prices below $1.00 trade in sub-pennies), which C must handle.
  *Proposed:* all three built and measured; the default chosen from the figures.
- **D3. Queue position.** *Proposed:* each level keeps its orders in time priority (an intrusive
  doubly linked list through the order records), so a consumer can see where an order stands.
  *Alternative:* totals only (shares and count per level): cheaper by the list upkeep, but loses
  what a market-by-order feed offers over a price-level feed.
- **D4. Capacity.** *Proposed:* every structure sized at start from configuration (orders live at
  once, instruments, levels per side), allocated and pre-faulted; nothing allocates while events
  are applied. Running out is not a crash: the event is refused, the instrument's book is marked
  unreliable (note 001's principle), and the count of such refusals is reported. The defaults
  come from the sample day's peaks, with headroom stated: 1,925,638 orders live at once (at 15:57,
  just before the close), Stock Locate codes up to 8,915 (the field is 16-bit), 5,482 price levels
  in the busiest book. *Proposed defaults:* 4 million orders, 65,536 instruments; the level
  capacity once D2's structures are built, from the peak of all books together.
- **D5. Bad events.** An Execute, Cancel or Delete for an unknown reference number, a Cancel for
  more shares than remain, an Add reusing a live reference number: each means a message was missed
  or misapplied. *Proposed:* never applied silently: counted, the book marked unreliable, and the
  event left for note 005's recovery to explain. The reference book flags the same events, so a
  difference between the two is a bug in one of them.
- **D6. Locked and crossed books.** Whether the displayed Nasdaq book can show a bid at or above
  the offer is not stated in the specification. On the sample day, after 4,821 order events a
  book was crossed and after 19 it was locked. 4,363 of the crossed (90%) and all the locked were
  in instruments halted, paused or in a quotation-only period (Stock Trading Action states `H`,
  `P`, `Q`), when orders rest without matching; 458 were in instruments trading (`T`). *Agreed:*
  the rule set from what is found. *Found* (Review): all 458 belong to books that became crossed
  during a halt or pause and were still crossed at the resumption, clearing within 55 to 149
  events. *The rule:* a book never becomes locked or crossed while its instrument is in state `T`;
  one crossed when trading resumes may stay so until it clears.
- **D7. Input for this note's tests and figures, before the decoder exists.** Note 004 builds the
  decoder, after this note. *Proposed:* a small extraction tool, outside the hot path and not
  part of the handler, writes the sample day's order events in sequence into a compact file of
  fixed-size records in `data/`; this note's tests and benchmarks read that. Note 004 then checks
  its own decoder produces the same events. Synthetic events alone would bias the measurement,
  as the gateway learnt (its note 002: cyclic inputs made decoding look faster than it is).

**The reference book.** `std::map` and `std::unordered_map`, no pools, no tricks; it keeps the
same queue order (D3) and flags the same bad events (D5). After every event, tests compare the
changed level and both bests; at checkpoints, every level of every book.

**Invariants**, checked after every event in tests: a level's shares are the sum of its orders'
and its count their number; no order has zero or negative shares; every order is in exactly one
level of its instrument's side; the best price is the best level present; plus D6's rule once
measured.

**How it is measured** (on the harness of `lockfree-ipc`, as the gateway's stage costs were): the
cost of applying each event, by type, each call timed, as percentiles, after a discarded
warm-up; the whole day's events applied as fast as possible, as events a second; memory in use
at the day's peak. The fast book beside the reference book, and each alternative of D1 and D2
beside the others.

## Spec

- S1. Applying the sample day's order events, the fast book equals the reference book after every
  event (the changed level and both bests) and in full at checkpoints, for every combination of
  D1 and D2 alternatives built.
- S2. The invariants hold after every event of the sample day and of every hand-built test.
- S3. Every bad event of D5 is counted and marks its book unreliable, in both books alike; tests
  cover each kind.
- S4. Applying an event makes no allocation or system call (checked in the compiled code, as in
  the gateway).
- S5. Per-event cost by type, whole-day throughput and peak memory are published for every D1 and
  D2 alternative and for the reference book, as ranges over at least three runs, via
  `run.py report`.
- S6. D6's finding on locked and crossed books, with its counts from the sample day, is recorded,
  and its rule is an invariant.

## Tasks

- [ ] The repo's build: CMake, warnings as errors, `lockfree-ipc` fetched at a pinned commit,
      CTest; CI as the gateway's (x64, TSan, ASan/UBSan, arm64)
- [ ] The sample day fetched by a script, size and SHA-256 checked (note 001)
- [x] D7: the extraction tool and the event file; counts of each event type (Review)
- [x] D1, D4, D6 from the data: reference-number density, peak live orders, instruments, levels,
      locked and crossed books; recorded here before the code (Review)
- [x] D6: explain the 458 crossed books in state `T` (Review)
- [x] The reference book; the invariants; hand-built tests of every event and every bad event (Review)
- [x] The fast book: D1 A–C, D2 A–C, D3, D4, D5
- [x] S1, S2 over the sample day; S4's call-graph check
- [ ] The benchmark and the report (S5)

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-25 | Draft | Written for review from TotalView-ITCH 5.0, sections 1.3 to 1.5 (read today: the order messages, and that `P`, `Q` and `B` do not change the displayed book). **The sample day, in figures** (30 January 2020, SHA-256 `8f5399155242f8d84338c36fdd78a47c0b0aa3df5707829b20c5158c258364c6`, 5,597,158,940 bytes as listed; read once by a throwaway program outside the repo, a plain map-based book, 152 s for the day): 423,285,709 messages, 12.95 GB uncompressed. Order events: Add 184,735,355 (`A`) and 1,875,350 (`F`), Delete 180,285,101, Replace 36,777,372, Executed 8,415,610, Cancel 4,990,972, Executed with Price 139,474. Every Execute, Cancel, Delete and Replace found its order (none unknown), and none was live at the end. Other types: `I` 4,025,192, `P` 1,779,727, `L` 216,802, `Q` 17,835, `Y` 9,068, `H` 8,921, `R` 8,916, `S` 6, `J` 5, `K` 2, `V` 1. Reference numbers, peaks and locked and crossed books: in D1, D4 and D6 |
| 2026-10-25 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 and D2 three ways each, chosen from the figures; D3 queue position kept; D4 fixed capacity, pre-faulted, unreliable rather than a crash when full; D5 bad events counted and marking the book unreliable; D6 locked or crossed only outside state `T`, once the 458 are explained; D7 an extraction tool and event file until the decoder exists |
| 2026-10-25 | Agreed (task) | **D7 built.** `include/itchfh/event.hpp` (the book's input: one 40-byte record per event; add, executed, executed with price, cancel, delete, replace, and trading state for D6), `include/itchfh/event_file.hpp` (a 64-byte header, then the records, mapped to read), `tools/extract_events.cpp` (each message's length checked against the specification's). The sample day: 417,228,155 events from 423,285,709 messages in 64 s, none of the wrong length, `data/01302020.events` 16.7 GB (ignored). Its counts equal, type by type, those of the independent throwaway reading earlier in this Review |
| 2026-10-25 | Agreed (D6) | **The 458 crossed books in state `T`, explained** (a throwaway program over the event file, a plain map-based book, outside the repo). Counted as episodes (crossed after an event, until no longer), the day has 10, and every one began while its instrument was halted (6) or paused (4); none began in `T`. Five uncrossed before the resumption; five were still crossed when the Stock Trading Action said `T`, and cleared 55, 55, 93, 111 and 149 events later (the reopening's executions and deletes): 463 events, of which 458 left the book crossed and the last five uncrossed it. The 19 locked moments were all in `P`. **D6's rule:** a book never becomes locked or crossed while its instrument is in state `T`; one crossed when trading resumes may remain so until it clears. An invariant in the tests |
| 2026-10-25 | Agreed (task) | **The reference book** (`include/itchfh/reference_book.hpp`): `std::map` levels per side, `std::list` queues, `std::unordered_map` orders; the shared types (`book_types.hpp`: level and order views, D5's bad events, what an event changed). **Invariants** (`tests/invariants.hpp`, through any book's public interface, so the fast book is held to the same): per level after every event, every level of every book at checkpoints, D6 followed event by event. `reference_book_test`: every event type and every bad event by hand, queue order, replace moving and requeuing, D6's watch both ways (26 checks); clean under ASan/UBSan and TSan. `sample_day_test` (label `data`): the reference book over all 417,228,155 events, invariants after every event, 41 full checkpoints and the end, D6, nothing refused, no order live at the close; 541 s |
| 2026-10-25 | Built | **The fast book** (`include/itchfh/book.hpp`): orders and levels in fixed pools with free lists, each level's queue a doubly linked list through the order records (D3), nothing changed until every capacity check has passed (D4: a replace that runs out of room has already removed its old order, so its book is marked unreliable, as stated in the code), bad events as the reference book's (D5). D1 (`ref_tables.hpp`): A `std::unordered_map`; B open addressing, linear probing, at most half full, backward-shift deletion (no tombstones); C a direct array over 2^29 numbers, B beyond. D2 (`levels.hpp`): A `std::map`; B a sorted array, best at the end, in doubling chunks of a pre-faulted arena; C a window of price keys per side (a key a price below $1.00, a cent a key above; off-grid prices have none) with a bitmap for the best, the rest in a B array; the window is moved only when its side is empty, so a price is never in both. **Tests.** `book_test`: all nine combinations in lockstep with the reference book over 60,000 random events each (bad events among them, sub-dollar, off-grid and far prices, few windows), every event's result, changed levels and bests compared, every level and queue every 500, invariants; D4's capacity refusals. Shown to catch three faults put in on purpose (the window's best ignoring its overflow, deletion without the shift, new orders queued at the front). **S1, S2:** `sample_day_test`, all nine against the reference book over the sample day: equal after every one of the 417,228,155 events and at 42 checkpoints (every level and queue), the reference book's invariants and D6 throughout, nothing refused, nothing live at the close; 5,676 s. The day's peaks: 1,925,638 orders and 782,863 levels live at once (D4's defaults, 4 million and 2 million, hold them). **S4:** `check_hot_path`, applying an event in the four combinations meant not to allocate (B and C of each): every function reached calls only memory functions and the stack protector, no system call; shown to fail for A with A (`std::map`'s node allocation). Clean under ASan/UBSan and TSan; CI green (x64, TSan, ASan/UBSan, arm64) |
| 2026-10-25 | Built (runs, not published) | **First three bench-mode runs** (`32978da`, clean; kept outside the repo): every book's p50 within 8 ns and throughput within 2% across them. Whole day, all events, p50 and events a second: reference book 574–582 ns, 1.82–1.84 M/s; refs A with levels A 319 ns, 2.96–2.97 M; refs B with levels B 173–174 ns, 6.21–6.32 M; refs C with levels B 138 ns, 8.92–8.96 M; refs C with levels C 136 ns, 9.55–9.61 M. Maxima: 0.4–1.35 ms wherever a standard container is used (allocation as it grows; the reference book's 172 ms is its hash table rehashing, as it reserves nothing), 17–40 µs for the fixed pools. **Not published: a flaw in the benchmark's memory figure.** It read resident memory at the end of the day, when nothing is live, so the reference book showed 22 MB; S5 asks for the peak. Fixed (agreed with Shep, option A): resident memory read every million events in the timed pass, between calls, its peak less what was resident before the book was built; on the first 20 million events the reference book's peak is 203 MB. Three runs again. **D1 and D2, the default** (agreed with Shep, option A, from these figures): refs C, the direct array (about 1.4 times open addressing's throughput, 35 ns less at p50, with either level structure), with levels B, sorted arrays (within 2 ns at p50 and 7% in throughput of the window, simpler, 264 MB less); `default_book` in `book.hpp`. The direct array's 2 GB is sized from this day's reference numbers (to 494 million): a day beyond its 2^29 falls back to open addressing for those orders, correct but slower |
| 2026-10-25 | Built (runs, not published) | **Second three runs** (`f1dc40d`, clean; kept outside the repo), agreeing with the first within the same bounds; the reference book's peak 229 MB. **Set aside for a second benchmark flaw:** the books on standard containers ran after the reference book in one process and reused the heap it had freed (glibc keeps freed memory for reuse), so their growth was hidden (refs A with levels A: 168 MB, its fixed pools alone) and their heap's page faults never taken, which may flatter their latency tails. Shown on the first 20 million events: that book alone in a fresh process adds 288 MB; after a freed reference book, 199 MB; with `malloc_trim(0)` first, 287 MB. Fixed (agreed with Shep, option A): the heap handed back before every pass of every book. The books on fixed tables (every B and C combination, the default among them) use fresh mappings and were not affected. Three runs again |

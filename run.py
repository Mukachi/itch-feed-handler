#!/usr/bin/env python3
"""Results report: RESULTS.md and the README's figures, generated from committed JSON.

  python run.py report          write RESULTS.md and fill the README's result blocks
  python run.py report --check  regenerate in memory; exit 1 naming any difference

As lockfree-ipc's report (its note 007) and the gateway's, and built on it: its
environment checker, conditions table and formatting are used from the copy
CMake fetched at the pinned commit (build/_deps/lockfree_ipc-src), so a run
here is held to the same rules. Only files listed in results/published.toml
are read, each validated first: environment checker, published, bench mode,
clean build, its component's block present, one commit, machine and kernel
command line per set. Figures are min-max over a set's runs. Standard library
only.
"""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

SETS = ("book", "handler", "decoder", "sequencer", "snapshot", "feed", "publish", "record")
BLOCK = re.compile(r"(<!-- results:(?P<name>[a-z0-9-]+) -->\n)(?P<body>.*?)(<!-- /results -->)", re.S)
MEAN = re.compile(r"\b(mean|average|avg)\b", re.I)


class ReportError(Exception):
    pass


def harness(root, lfipc=None):
    """lockfree-ipc's run.py, from the fetched copy, as a module."""
    path = (Path(lfipc) if lfipc else root / "build" / "_deps" / "lockfree_ipc-src") / "run.py"
    if not path.is_file():
        raise ReportError(f"{path} missing: configure the build first (cmake -S . -B build)")
    spec = importlib.util.spec_from_file_location("lfipc_run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod, path.parent


def validate(root, lf_root, name, files):
    runs = []
    for f in files:
        path = root / "results" / f
        if not path.is_file():
            raise ReportError(f"{name}: {f}: missing")
        check = subprocess.run([sys.executable, "-I", str(lf_root / "tests" / "check_environment_json.py"), str(path)],
                               capture_output=True, text=True)
        if check.returncode != 0:
            fails = [l for l in check.stdout.splitlines() if l.startswith("FAIL")]
            raise ReportError(f"{name}: {f}: environment check failed: {fails[:2]}")
        d = json.loads(path.read_text())
        e = d["environment"]
        if not (e["published"] and e["ok"] and e["bench_mode"]["ok"]):
            raise ReportError(f"{name}: {f}: not a published run (refused or outside results/)")
        if not e["build"]["clean"]:
            raise ReportError(f"{name}: {f}: built from a tree with uncommitted changes")
        if name not in d or not d.get("clock", {}).get("ok"):
            raise ReportError(f"{name}: {f}: no {name} block, or the clock refused")
        runs.append((f, d))
    if not runs:
        raise ReportError(f"{name}: no runs listed")
    for key, get in (("commit", lambda e: e["build"]["commit"]),
                     ("machine", lambda e: (e["machine"]["product"], e["cpu"]["model"], e["cpu"]["microcode"])),
                     ("kernel command line", lambda e: e["kernel"]["cmdline"])):
        if len({get(d["environment"]) for _, d in runs}) > 1:
            raise ReportError(f"{name}: runs mix more than one {key}")
    return runs


def load_sets(root, lf_root):
    manifest = root / "results" / "published.toml"
    if not manifest.is_file():
        return {}
    m = tomllib.loads(manifest.read_text())
    unknown = set(m) - set(SETS)
    if unknown:
        raise ReportError(f"published.toml: unknown sets {sorted(unknown)}")
    return {n: (m[n].get("note", ""), validate(root, lf_root, n, m[n]["runs"])) for n in SETS if n in m}


# ---------------------------------------------------------------- note 002


def book_groups(runs):
    """Each book's entries across the runs, in the benchmark's order."""
    names = [b["name"] for b in runs[0][1]["book"]["books"]]
    for _, d in runs:
        if [b["name"] for b in d["book"]["books"]] != names:
            raise ReportError("book: runs measured different books")
        if not all(b["nothing_refused"] for b in d["book"]["books"]):
            raise ReportError("book: a run refused events (a book differs from the day)")
        if d["book"]["source"] != runs[0][1]["book"]["source"]:
            raise ReportError("book: runs read different days")
    return [(n, [d["book"]["books"][i] for _, d in runs]) for i, n in enumerate(names)]


def label(b):
    return "reference book" if b["refs"] == "reference" else f"refs {b['refs']}, levels {b['levels']}"


def book_rows(lf, runs):
    rows = []
    for _, bs in book_groups(runs):
        all_ = [next(t for t in b["types"] if t["type"] == "all") for b in bs]
        rows.append([label(bs[0]), lf.rng([b["events_per_s"] / 1e6 for b in bs], lambda v: f"{v:.2f}")]
                    + [lf.rng([t[f] for t in all_]) for f in ("p50_ns", "p99_ns", "p999_ns", "max_ns")]
                    + [lf.rng([b["peak_resident_mb"] for b in bs], lambda v: f"{v:,.0f}")])
    return rows


def type_rows(lf, runs):
    rows = []
    for _, bs in book_groups(runs):
        for t0 in bs[0]["types"]:
            if t0["type"] == "all":
                continue
            ts = [next(t for t in b["types"] if t["type"] == t0["type"]) for b in bs]
            cells = [lf.rng([t[f] for t in ts if t[f] >= 0]) for f in ("p50_ns", "p99_ns", "p999_ns")]
            rows.append([label(bs[0]), t0["type"], f"{t0['count']:,}"] + cells)
    return rows


def book_section(lf, note, runs):
    b0 = runs[0][1]["book"]
    none = lf.rng([d["book"]["none"]["p50_ns"] for _, d in runs])
    return [f"## The order book (note {note})", "",
            f"Nasdaq TotalView-ITCH 5.0, the sample day `{b0['source']}`: {b0['events']:,} order and trading-state events, "
            "applied in feed order on one pinned CPU (CPU 2) by each book in turn, every book fresh for each of two passes. "
            "Events a second: the whole day applied untimed. Per event: each apply timed between start() and stop(), "
            f"unsubtracted (the clock's own cost, an empty window, is {none} ns at p50), into a histogram, the first "
            f"{b0['warmup_discarded']:,} events discarded as warm-up. Peak MB: the most resident memory the book added over "
            "the day, read every million events between calls (the fast books' tables are allocated and faulted in at "
            "construction, so they count whole). The events are copied into memory "
            f"bound to the measured CPU's node first. {len(runs)} runs; ranges over them.", "",
            lf.table(["Book", "M events/s", "p50", "p99", "p99.9", "max", "Peak MB"], book_rows(lf, runs)), "",
            "By event type (p99.9 left out where fewer than ten events lie above it):", "",
            lf.table(["Book", "Event", "Events", "p50", "p99", "p99.9"], type_rows(lf, runs)), ""]


# ---------------------------------------------------------------- note 003

TYPE_NAMES = {"all": "all", "S": "System Event", "R": "Stock Directory", "H": "Stock Trading Action", "Y": "Reg SHO",
              "L": "Market Participant Position", "V": "MWCB Decline Level", "W": "MWCB Status", "K": "IPO Quoting Period",
              "J": "LULD Auction Collar", "h": "Operational Halt", "A": "Add Order", "F": "Add Order, attributed",
              "E": "Order Executed", "C": "Order Executed with Price", "X": "Order Cancel", "D": "Order Delete",
              "U": "Order Replace", "P": "Trade (non-displayed)", "Q": "Cross Trade", "B": "Broken Trade", "I": "NOII",
              "N": "Retail Interest", "O": "Direct Listing"}


def handler_groups(runs):
    names = [x["name"] for x in runs[0][1]["handler"]["setups"]]
    for _, d in runs:
        if [x["name"] for x in d["handler"]["setups"]] != names:
            raise ReportError("handler: runs measured different setups")
        if not all(x["clean"] for x in d["handler"]["setups"]) or d["handler"]["undecoded"]:
            raise ReportError("handler: a run refused messages or left some undecoded")
    return [(n, [d["handler"]["setups"][i] for _, d in runs]) for i, n in enumerate(names)]


def handler_rows(lf, runs):
    rows = []
    for name, xs in handler_groups(runs):
        all_ = [next(t for t in x["types"] if t["type"] == "all") for x in xs]
        rows.append([name, lf.rng([x["messages_per_s"] / 1e6 for x in xs], lambda v: f"{v:.2f}")]
                    + [lf.rng([t[f] for t in all_]) for f in ("p50_ns", "p99_ns", "p999_ns", "max_ns")]
                    + [lf.rng([x["peak_resident_mb"] for x in xs], lambda v: f"{v:,.0f}")])
    return rows


def handler_type_rows(lf, runs):
    rows = []
    for name, xs in handler_groups(runs):
        for t0 in xs[0]["types"]:
            if t0["type"] == "all":
                continue
            ts = [next(t for t in x["types"] if t["type"] == t0["type"]) for x in xs]
            cells = [lf.rng([t[f] for t in ts if t[f] >= 0]) for f in ("p50_ns", "p99_ns", "p999_ns")]
            rows.append([name, f"`{t0['type']}` {TYPE_NAMES.get(t0['type'], '')}", f"{t0['in_day']:,}"] + cells)
    return rows


def handler_section(lf, note, runs):
    h0 = runs[0][1]["handler"]
    none = lf.rng([d["handler"]["none"]["p50_ns"] for _, d in runs])
    return [f"## The event handler (note {note})", "",
            f"Every message of the sample day `{h0['source']}` ({h0['messages']:,}), decoded first by the reference decoder into "
            "memory bound to the measured CPU's node, then handled on one pinned CPU (CPU 2): the handler with the default book "
            "(note 002) behind it, beside the reference handler with the reference book; each fresh for each of two passes, the "
            "heap handed back before each. Messages a second: the whole day handled untimed. Per message: each call timed "
            f"between start() and stop(), unsubtracted (the clock's own cost is {none} ns at p50), the first "
            f"{h0['warmup_discarded']:,} messages discarded as warm-up. Peak MB: the most resident memory added over the day. "
            f"The sink counts its calls and does nothing else. {len(runs)} runs; ranges over them.", "",
            lf.table(["Setup", "M messages/s", "p50", "p99", "p99.9", "max", "Peak MB"], handler_rows(lf, runs)), "",
            "By message type (p99.9 left out where fewer than ten messages lie above it):", "",
            lf.table(["Setup", "Message", "In the day", "p50", "p99", "p99.9"], handler_type_rows(lf, runs)), ""]


# ---------------------------------------------------------------- note 004


def decoder_groups(runs):
    names = [x["name"] for x in runs[0][1]["decoder"]["decoders"]]
    for _, d in runs:
        if [x["name"] for x in d["decoder"]["decoders"]] != names:
            raise ReportError("decoder: runs measured different decoders")
        if any(x["refused"] for x in d["decoder"]["decoders"]):
            raise ReportError("decoder: a run refused messages of the day")
    return [(n, [d["decoder"]["decoders"][i] for _, d in runs]) for i, n in enumerate(names)]


def decoder_rows(lf, runs):
    rows = []
    for name, xs in decoder_groups(runs):
        all_ = [next(t for t in x["types"] if t["type"] == "all") for x in xs]
        mis = [x["mispredictions_per_message"] for x in xs if x["mispredictions_per_message"] >= 0]
        rows.append([name, lf.rng([x["messages_per_s"] / 1e6 for x in xs], lambda v: f"{v:.1f}")]
                    + [lf.rng([t[f] for t in all_]) for f in ("p50_ns", "p99_ns", "p999_ns")]
                    + [lf.rng(mis, lambda v: f"{v:.3f}") if mis else "–",
                       lf.rng([x["end_to_end_messages_per_s"] / 1e6 for x in xs], lambda v: f"{v:.2f}"),
                       lf.rng([x["end_to_end"]["p50_ns"] for x in xs]), lf.rng([x["end_to_end"]["p99_ns"] for x in xs])])
    return rows


def decoder_type_rows(lf, runs):
    rows = []
    for name, xs in decoder_groups(runs):
        for t0 in xs[0]["types"]:
            if t0["type"] == "all":
                continue
            ts = [next(t for t in x["types"] if t["type"] == t0["type"]) for x in xs]
            rows.append([name, f"`{t0['type']}` {TYPE_NAMES.get(t0['type'], '')}", f"{t0['in_day']:,}"]
                        + [lf.rng([t[f] for t in ts if t[f] >= 0]) for f in ("p50_ns", "p99_ns")])
    return rows


DECODER_HEAD = ["Decoder", "M messages/s", "p50", "p99", "p99.9", "Mispred./msg", "With handler M/s", "With handler p50",
                "With handler p99"]


def decoder_section(lf, note, runs):
    d0 = runs[0][1]["decoder"]
    none = lf.rng([d["decoder"]["none"]["p50_ns"] for _, d in runs])
    return [f"## The decoder (note {note})", "",
            f"Every message of the sample day `{d0['source']}` ({d0['messages']:,}), held uncompressed in memory bound to the "
            "measured CPU's node as the file has it (each message after its length, so the next message's bytes follow it: the "
            "padded contract), decoded in place in the day's order on one pinned CPU (CPU 2) by the reference decoder and each "
            "alternative. Messages a second: the whole day decoded untimed. Per message: each decode timed between start() and "
            f"stop(), unsubtracted (the clock's own cost is {none} ns at p50), the first {d0['warmup_discarded']:,} messages "
            "discarded as warm-up; branch mispredictions per message counted over that pass. With handler: every message decoded "
            "and handled (note 003's handler on the default book, a sink that counts), untimed for the rate, then each "
            f"decode-and-handle timed. {len(runs)} runs; ranges over them.", "",
            lf.table(DECODER_HEAD, decoder_rows(lf, runs)), "",
            "By message type:", "",
            lf.table(["Decoder", "Message", "In the day", "p50", "p99"], decoder_type_rows(lf, runs)), ""]


# ---------------------------------------------------------------- note 005


def sequencer_rows(lf, runs):
    q = [d["sequencer"] for _, d in runs]
    rows = [["in order, a packet", lf.rng([x["packets_per_s"] / 1e6 for x in q], lambda v: f"{v:.2f} M packets/s") + ", "
             + lf.rng([x["messages_per_s"] / 1e6 for x in q], lambda v: f"{v:.1f} M messages/s")]
            + [lf.rng([x["in_order"][f] for x in q]) for f in ("p50_ns", "p99_ns", "p999_ns")],
            ["held after a gap, a packet", "–"] + [lf.rng([x["held"][f] for x in q]) for f in ("p50_ns", "p99_ns", "p999_ns")]]
    for i, b0 in enumerate(q[0]["backlogs"]):
        bs = [x["backlogs"][i] for x in q]
        rows.append([f"the gap filled, {b0['held_packets']:,} held delivered",
                     lf.rng([b["median_ns_per_packet"] for b in bs], lambda v: f"{v:,.0f}") + " ns a packet"]
                    + [lf.rng([b[f] for b in bs if b[f] >= 0]) for f in ("p50_ns", "p99_ns", "p999_ns")])
    return rows


SEQUENCER_HEAD = ["Measure", "Rate", "p50", "p99", "p99.9"]


def sequencer_section(lf, note, runs):
    q0 = runs[0][1]["sequencer"]
    none = lf.rng([d["sequencer"]["none"]["p50_ns"] for _, d in runs])
    return [f"## Sequencing and recovery (note {note})", "",
            f"The sample day `{q0['source']}` packed by the packetizer into {q0['packets']:,} MoldUDP64 packets of at most 1,400 "
            "bytes of messages, held in memory bound to the measured CPU's node, through the sequencer on one pinned CPU (CPU 2), "
            "its output counting messages and nothing else. In order: every packet, each call timed between start() and stop(), "
            f"unsubtracted (the clock's own cost is {none} ns at p50), the first {q0['warmup_discarded']:,} discarded; the rate "
            "from an untimed pass. Held: a packet arriving after a gap, copied into the hold. The gap filled: one packet missing "
            "with that many held behind it, then the missing packet, the call that delivers the whole backlog timed (200 trials "
            "at each size, spread over the day; p99 and p99.9 left out where fewer than ten lie above them). What is measured is "
            "the sequencer's own work; recovery over a network is note 007's and note 010's. Holding and delivering a backlog "
            "came out at two levels across runs, about 20% apart, with the same instructions, mispredictions and (in huge "
            "pages) next to no TLB misses; the ranges span both (note 005, Review). "
            f"{len(runs)} runs; ranges over them.", "",
            lf.table(SEQUENCER_HEAD, sequencer_rows(lf, runs)), ""]


# ---------------------------------------------------------------- note 006


def snapshot_rows(lf, runs):
    pts = [d["snapshot"]["points"] for _, d in runs]
    for ps in pts:
        if [p["at"] for p in ps] != [p["at"] for p in pts[0]] or not all(p["ok"] for p in ps):
            raise ReportError("snapshot: runs took different points, or a client did not match the server")
    rows = []
    for i, p0 in enumerate(pts[0]):
        xs = [ps[i] for ps in pts]
        rows.append([f"{p0['at']:,}" + (" (the peak)" if p0["peak"] else ""), f"{p0['live_orders']:,}", f"{p0['messages']:,}",
                     f"{p0['bytes'] / 1e6:,.0f}", lf.rng([x["build_ms"] for x in xs], lambda v: f"{v:,.0f}"),
                     lf.rng([x["apply_ms"] for x in xs], lambda v: f"{v:,.0f}")])
    return rows


SNAPSHOT_HEAD = ["At message", "Live orders", "Spin messages", "Spin MB", "Build ms", "Apply ms"]


def snapshot_section(lf, note, runs):
    s0 = runs[0][1]["snapshot"]
    return [f"## Snapshots (note {note})", "",
            f"The sample day `{s0['source']}` followed by the snapshot server (the reference handler and book); at every 50 million "
            "messages and at the day's peak of live orders, a GLIMPSE 5.0 spin over SoupBinTCP built (the server's cost) and "
            "applied by a client, read, decoded and handled into a fresh default book (building that book is not counted), on one "
            "pinned CPU (CPU 2). Applying is what a recovery costs before the held feed is caught up (the sequencer's backlog "
            f"figures). Every client's books equal to the server's. {len(runs)} runs; ranges over them.", "",
            lf.table(SNAPSHOT_HEAD, snapshot_rows(lf, runs)), ""]


# ---------------------------------------------------------------- note 007

FEED_PATHS = {"loopback": "loopback", "card": "the card", "card-busy-poll": "the card, kernel busy polling"}


def feed_by_path(runs):
    paths = {}
    for _, d in runs:
        paths.setdefault(d["feed"]["path"], []).append(d["feed"])
    for p, fs in paths.items():
        keys = [[(c["arrangement"], c["pace"]) for c in f["cases"]] for f in fs]
        if any(k != keys[0] for k in keys) or not all(c["ok"] or not c["sustained"] for f in fs for c in f["cases"]):
            raise ReportError(f"feed: {p} runs took different cases, or a sustained case did not end with every message")
    return paths


def feed_rows(lf, fs):
    rows = []
    for i, c0 in enumerate(fs[0]["cases"]):
        cs = [f["cases"][i] for f in fs]
        sus = [c for c in cs if c["sustained"]]
        rows.append([c0["arrangement"], lf.rate(c0["pace"]), lf.rng([round(c["achieved"], -2) for c in cs], lf.rate),
                     lf.rng([c["kernel_drops"] for c in cs], lambda v: f"{v:,}")]
                    + [lf.rng([c["cost"][f] for c in sus]) for f in ("p50_ns", "p99_ns")]
                    + [lf.rng([c["kernel_to_handled"][f] for c in sus]) for f in ("p50_ns", "p99_ns", "p999_ns")])
    return rows


FEED_HEAD = ["Arrangement", "Pace", "Achieved", "Kernel drops", "Cost p50", "Cost p99", "Kernel to handled p50", "p99", "p99.9"]


KEEPS_UP_P99_NS = 100_000  # S6 amended: the receiver keeps up while kernel to handled p99 stays under 100 us


def highest(fs, a, ok):
    # Each run's highest pace meeting `ok` for arrangement `a`, the lowest of those over the runs.
    return min(max([c["pace"] for c in f["cases"] if c["arrangement"] == a and ok(c)], default=0) for f in fs)


def feed_summary_rows(lf, paths):
    rows = []
    for p, fs in paths.items():
        for a in dict.fromkeys(c["arrangement"] for c in fs[0]["cases"]):
            no_drops = highest(fs, a, lambda c: c["sustained"])
            keeps_up = highest(fs, a, lambda c: c["sustained"] and c["kernel_to_handled"]["p99_ns"] < KEEPS_UP_P99_NS)
            at = [c for f in fs for c in f["cases"] if c["arrangement"] == a and c["pace"] == 100_000 and c["sustained"]]
            rows.append([FEED_PATHS.get(p, p), a, lf.rate(keeps_up) if keeps_up else "–", lf.rate(no_drops) if no_drops else "–",
                         lf.rng([c["cost"]["p50_ns"] for c in at])]
                        + [lf.rng([c["kernel_to_handled"][f] for c in at]) for f in ("p50_ns", "p99_ns")])
    return rows


FEED_SUMMARY_HEAD = ["Path", "Arrangement", "Keeps up (p99 < 100 µs)", "No kernel drops", "Cost p50 at 100k/s",
                     "Kernel to handled p50 at 100k/s", "p99"]


def feed_section(lf, note, runs):
    paths = feed_by_path(runs)
    f0 = runs[0][1]["feed"]
    lines = [f"## Receiving the feed (note {note})", "",
             f"The first {f0['messages']:,} messages of the sample day (about 1.16 million MoldUDP64 packets) sent by our sender "
             "(its own process, on its own reserved CPU; for the card, in the far port's network namespace) onto a multicast "
             "group at each pace, into each arrangement: one thread (CPU 2) reading in batches (`recvmmsg`) or a datagram a call "
             "(`recvmsg`), or two threads (receiving on CPU 2, handling on CPU 3, lockfree-ipc's SPSC ring between), behind them "
             "the default decoder and the handler on the default book. Achieved: the sender's packets over its first to last "
             "sending. Kernel drops: the socket's own count (`SO_RXQ_OVFL`), receive buffer "
             f"{f0['buffer_granted'] // 1024:,} KB as the kernel reports it (4 MB asked, `rmem_max` {int(f0['rmem_max']) // 1024:,} KB; "
             "the kernel doubles it for its own bookkeeping); a case with drops is overloaded and its costs are left out. No kernel "
             "drops is not the same as keeping up: near the limit the receiver falls behind and the buffer holds the backlog for "
             "the ten seconds or so a case lasts (kernel to handled p99 in milliseconds), and a longer run would drop; so the "
             "summary also gives the highest pace at which the receiver keeps up, kernel to handled p99 under 100 µs, each the "
             "lowest over the runs. Cost: each receiving pass that found datagrams, timed between start() and stop(), "
             "divided among them (one thread: everything to the handler; two: the receiving side alone). Kernel to handled: a "
             "datagram's kernel receive stamp (`SO_TIMESTAMPNS`) to its first message handled (`CLOCK_REALTIME`). The first "
             f"{f0['warmup_discarded']:,} of each discarded. Ranges over runs.", ""]
    for p, fs in paths.items():
        lines += [f"### Over {FEED_PATHS.get(p, p)} ({len(fs)} runs; `busy_read` {fs[0]['busy_read']}, `busy_poll` {fs[0]['busy_poll']})", "",
                  lf.table(FEED_HEAD, feed_rows(lf, fs)), ""]
    return lines


# ---------------------------------------------------------------- note 008


def publish_rows(lf, runs):
    ps = [d["publish"] for _, d in runs]
    names = [c["name"] for c in ps[0]["cases"]]
    for p in ps:
        if [c["name"] for c in p["cases"]] != names or any(c["overruns"] for c in p["cases"]):
            raise ReportError("publish: runs took different cases, or a consumer was overrun")
    rows = []
    for i, name in enumerate(names):
        cs = [p["cases"][i] for p in ps]
        first = [c["write_to_read_paced"][0] for c in cs if c["write_to_read_paced"]]
        last = [c["write_to_read_paced"][-1] for c in cs if c["write_to_read_paced"]]
        rows.append([name, lf.rng([c["messages_per_s"] / 1e6 for c in cs], lambda v: f"{v:.2f}")]
                    + [lf.rng([c["cost"][f] for c in cs]) for f in ("p50_ns", "p99_ns", "p999_ns")]
                    + [lf.rng([w[f] for w in first]) if first else "–" for f in ("p50_ns", "p99_ns")]
                    + [lf.rng([w["p50_ns"] for w in last]) if len(cs[0]["write_to_read_paced"]) > 1 else "–"])
    return rows


PUBLISH_HEAD = ["Case", "M messages/s", "p50", "p99", "p99.9", "Write to read p50, first consumer",
                "p99", "Write to read p50, last consumer"]


def publish_section(lf, note, runs):
    p0 = runs[0][1]["publish"]
    return [f"## Publication (note {note})", "",
            f"The first {p0['messages']:,} messages of the sample day `{p0['source']}`, decoded into memory first, through a fresh "
            "default book and handler on one pinned CPU (CPU 2): with a sink that does nothing (no publisher), with the publisher "
            "and its state table only, and with 1, 2 and 4 consumers, each a process of its own on CPUs "
            f"{', '.join(str(c) for c in p0['consumer_cpus'])} (separate cores, the same node) reading its ring. Each message's "
            "handling timed between start() and stop(), unsubtracted, the first "
            f"{p0['warmup_discarded']:,} discarded. The publisher's cost is read from the absolute figures, not as a difference: "
            "with no publisher the p50 spread from run to run (mostly 132–138 ns, once 149 and once 178, the same binary and "
            "data), while the cases with the publisher stayed within a few ns (208 with the table only in every run), so a "
            "difference would carry the baseline's spread (note 008, Review). Write to read: "
            "the publishing thread stamps the cycle counter for each message's sequence number before handling it; a consumer "
            f"subtracts it on reading the update (so it includes the handling), paced at {p0['paced_rate'] / 1e6:g} million "
            f"messages a second over {p0['paced_messages']:,} messages; consumers are pushed to in order, so the first and the "
            f"last are shown. No consumer was overrun. {len(runs)} runs; ranges over them.", "",
            lf.table(PUBLISH_HEAD, publish_rows(lf, runs)), ""]


# ---------------------------------------------------------------- note 009


def record_rows(lf, runs):
    rs = [d["record"] for _, d in runs]
    keys = [(c["pace"], c["recording"]) for c in rs[0]["feed"]]
    for r in rs:
        if [(c["pace"], c["recording"]) for c in r["feed"]] != keys or not all(c["ok"] and c["unrecorded"] == 0 for c in r["feed"]):
            raise ReportError("record: runs took different cases, or a feed case did not end whole, or left datagrams unrecorded")
    rows = []
    for i, (pace, recording) in enumerate(keys):
        cs = [r["feed"][i] for r in rs]
        rows.append([lf.rate(pace), "on" if recording else "off"]
                    + [lf.rng([c["cost"][f] for c in cs]) for f in ("p50_ns", "p99_ns")]
                    + [lf.rng([c["kernel_to_handled"][f] for c in cs]) for f in ("p50_ns", "p99_ns", "p999_ns")]
                    + [lf.rng([c["kernel_drops"] for c in cs], lambda v: f"{v:,}")])
    return rows


RECORD_HEAD = ["Pace", "Recording", "Cost a datagram p50", "p99", "Kernel to handled p50", "p99", "p99.9", "Kernel drops"]


def record_flat_rows(lf, runs):
    fs = [d["record"]["flat_out"] for _, d in runs]
    return [[lf.rng([f[k] for f in fs]) for k in ("p50_ns", "p99_ns", "p999_ns")]
            + [lf.rng([f["mb_per_s"] for f in fs], lambda v: f"{v:,.0f}"),
               lf.rng([100 * f["unrecorded"] / f["calls"] for f in fs], lambda v: f"{v:.1f}%")]]


RECORD_FLAT_HEAD = ["record() p50", "p99", "p99.9", "Written MB/s", "Unrecorded"]


def record_section(lf, note, runs):
    r0 = runs[0][1]["record"]
    f0 = r0["flat_out"]
    return [f"## Recording (note {note})", "",
            f"The recorder on CPU {r0['cpu']} (the receive thread), its writer on housekeeping CPU {r0['writer_cpu']}, the "
            "recording on the build directory's disk (ext4 on one SAS disk behind a battery-backed controller cache). Writer flat out: "
            f"{f0['datagram_bytes']:,}-byte datagrams recorded in a tight loop for {f0['seconds']:.0f} s, each record() call timed "
            "between start() and stop(), unsubtracted; written: the bytes on disk over the time; unrecorded: the share of calls "
            "that found the ring full once the writer fell behind (the receive side never waits).", "",
            lf.table(RECORD_FLAT_HEAD, record_flat_rows(lf, runs)), "",
            f"The feed, recording off and on: the first {r0['messages']:,} messages of the sample day from our sender over "
            "loopback, one thread reading a datagram a call (note 007's default), behind it the default decoder and the handler; "
            "cost a datagram and kernel to handled as note 007 measures them. No datagram was left unrecorded. "
            f"{len(runs)} runs; ranges over them.", "",
            lf.table(RECORD_HEAD, record_rows(lf, runs)), ""]


def results_md(lf, sets):
    lines = ["# Results", "",
             "Generated by `python run.py report` from the runs listed in `results/published.toml`. "
             "Do not edit: `python run.py report --check` fails if this file and the JSON disagree. "
             "Latencies in ns unless marked µs; percentiles only.", ""]
    if not sets:
        return "\n".join(lines + ["No published results yet.", ""]).rstrip() + "\n"
    lines += lf.conditions(sets)
    if "book" in sets:
        lines += book_section(lf, *sets["book"])
    if "handler" in sets:
        lines += handler_section(lf, *sets["handler"])
    if "decoder" in sets:
        lines += decoder_section(lf, *sets["decoder"])
    if "sequencer" in sets:
        lines += sequencer_section(lf, *sets["sequencer"])
    if "snapshot" in sets:
        lines += snapshot_section(lf, *sets["snapshot"])
    if "feed" in sets:
        lines += feed_section(lf, *sets["feed"])
    if "publish" in sets:
        lines += publish_section(lf, *sets["publish"])
    if "record" in sets:
        lines += record_section(lf, *sets["record"])
    return "\n".join(lines).rstrip() + "\n"


def readme_blocks(lf, sets):
    blocks = {"handler-summary": "Event handler figures: not yet measured (design note 003).\n",
              "decoder-summary": "Decoder figures: not yet measured (design note 004).\n",
              "sequencer-summary": "Sequencer figures: not yet measured (design note 005).\n",
              "snapshot-summary": "Snapshot figures: not yet measured (design note 006).\n",
              "feed-summary": "Receiving figures: not yet measured (design note 007).\n",
              "publish-summary": "Publication figures: not yet measured (design note 008).\n",
              "record-summary": "Recording figures: not yet measured (design note 009).\n"}
    if "record" in sets:
        runs = sets["record"][1]
        blocks["record-summary"] = (lf.table(RECORD_HEAD, record_rows(lf, runs))
                                    + "\n\nThe receive path with the recorder off and on: one thread over loopback, ns, range over runs. "
                                    "The writer flat out and the method: [RESULTS.md](RESULTS.md).\n")
    if "publish" in sets:
        runs = sets["publish"][1]
        blocks["publish-summary"] = (lf.table(PUBLISH_HEAD, publish_rows(lf, runs))
                                     + f"\n\nThe sample day's first {runs[0][1]['publish']['messages']:,} messages through the handler and "
                                     "its publisher on one CPU, consumers in processes of their own; ns, range over runs. The method: "
                                     "[RESULTS.md](RESULTS.md).\n")
    if "feed" in sets:
        runs = sets["feed"][1]
        blocks["feed-summary"] = (lf.table(FEED_SUMMARY_HEAD, feed_summary_rows(lf, feed_by_path(runs)))
                                  + f"\n\nThe first {runs[0][1]['feed']['messages']:,} messages of the sample day over MoldUDP64 "
                                  "multicast from our sender, received, sequenced, decoded and handled on the default book, in packets "
                                  "a second (about 43 messages each); ns, range over runs; cost a datagram, for two threads the receiving side's alone. No kernel drops: over about ten seconds, the "
                                  "socket's buffer holding any backlog. Every pace and the method: [RESULTS.md](RESULTS.md).\n")
    if "snapshot" in sets:
        runs = sets["snapshot"][1]
        rows = [r for r in snapshot_rows(lf, runs) if "peak" in r[0]]
        blocks["snapshot-summary"] = (lf.table(SNAPSHOT_HEAD, rows)
                                      + f"\n\nA GLIMPSE 5.0 snapshot at the sample day's peak of live orders: built by the snapshot server, "
                                      f"applied by a client into a fresh book, one CPU, range over {len(runs)} runs. Every point of the "
                                      "day: [RESULTS.md](RESULTS.md).\n")
    if "sequencer" in sets:
        runs = sets["sequencer"][1]
        blocks["sequencer-summary"] = (lf.table(SEQUENCER_HEAD, sequencer_rows(lf, runs))
                                       + f"\n\nThe sample day in {runs[0][1]['sequencer']['packets']:,} MoldUDP64 packets, one CPU, ns, range "
                                       f"over {len(runs)} runs; the sequencer's own work. The method: [RESULTS.md](RESULTS.md).\n")
    if "decoder" in sets:
        runs = sets["decoder"][1]
        blocks["decoder-summary"] = (lf.table(DECODER_HEAD, decoder_rows(lf, runs))
                                     + f"\n\nEvery message of the sample day ({runs[0][1]['decoder']['messages']:,}) in its order, one CPU, "
                                     f"ns per message, range over {len(runs)} runs; with handler: decoded and handled on the default book. "
                                     "By message type and the method: [RESULTS.md](RESULTS.md).\n")
    if "handler" in sets:
        runs = sets["handler"][1]
        blocks["handler-summary"] = (lf.table(["Setup", "M messages/s", "p50", "p99", "p99.9", "max", "Peak MB"], handler_rows(lf, runs))
                                     + f"\n\nEvery message of the sample day ({runs[0][1]['handler']['messages']:,}), already decoded, "
                                     f"one CPU, ns per message, range over {len(runs)} runs. By message type and the method: "
                                     "[RESULTS.md](RESULTS.md).\n")
    if "book" not in sets:
        blocks["book-summary"] = "Order book figures: not yet measured (design note 002).\n"
        return blocks
    runs = sets["book"][1]
    blocks["book-summary"] = (lf.table(["Book", "M events/s", "p50", "p99", "p99.9", "max", "Peak MB"], book_rows(lf, runs))
            + f"\n\nThe sample day's {runs[0][1]['book']['events']:,} events, one CPU, ns per event, range over "
              f"{len(runs)} runs. By event type, the method and the conditions: [RESULTS.md](RESULTS.md).\n")
    return blocks


def report(root, check, lfipc=None):
    lf, lf_root = harness(root, lfipc)
    sets = load_sets(root, lf_root)
    results = results_md(lf, sets)
    readme_path = root / "README.md"
    readme = readme_path.read_text()
    blocks = readme_blocks(lf, sets)
    found = {m["name"] for m in BLOCK.finditer(readme)}
    if set(blocks) - found:
        raise ReportError(f"README.md has no block for {sorted(set(blocks) - found)}")
    new_readme = BLOCK.sub(lambda m: m.group(1) + blocks.get(m["name"], m["body"]) + m.group(4), readme)
    for text, what in ((results, "RESULTS.md"), (new_readme, "README.md")):
        if MEAN.findall(text):
            raise ReportError(f"{what}: generated text mentions {MEAN.findall(text)}")
    if not check:
        (root / "RESULTS.md").write_text(results)
        readme_path.write_text(new_readme)
        print(f"wrote RESULTS.md and {len(blocks)} README block(s)")
        return 0
    diffs = []
    current = (root / "RESULTS.md").read_text() if (root / "RESULTS.md").is_file() else ""
    if current != results:
        diffs.append("RESULTS.md")
    old = {m["name"]: m["body"] for m in BLOCK.finditer(readme)}
    new = {m["name"]: m["body"] for m in BLOCK.finditer(new_readme)}
    diffs += [f"README.md block '{k}'" for k in new if old.get(k) != new[k]]
    if diffs:
        print("report --check: out of date: " + ", ".join(diffs) + " (run: python run.py report)")
        return 1
    print("report --check: RESULTS.md and README.md agree with the published JSON")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("report", help="generate RESULTS.md and README blocks from published JSON")
    rp.add_argument("--check", action="store_true", help="compare only; exit 1 on any difference")
    rp.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help=argparse.SUPPRESS)
    rp.add_argument("--lockfree-ipc", dest="lfipc", help="the fetched lockfree-ipc (default: build/_deps/lockfree_ipc-src)")
    args = ap.parse_args()
    try:
        return report(args.root, args.check, args.lfipc)
    except ReportError as e:
        print(f"report: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

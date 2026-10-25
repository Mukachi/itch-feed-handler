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

SETS = ("book",)
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
    return "\n".join(lines).rstrip() + "\n"


def readme_blocks(lf, sets):
    if "book" not in sets:
        return {"book-summary": "Order book figures: not yet measured (design note 002).\n"}
    runs = sets["book"][1]
    return {"book-summary": lf.table(["Book", "M events/s", "p50", "p99", "p99.9", "max", "Peak MB"], book_rows(lf, runs))
            + f"\n\nThe sample day's {runs[0][1]['book']['events']:,} events, one CPU, ns per event, range over "
              f"{len(runs)} runs. By event type, the method and the conditions: [RESULTS.md](RESULTS.md).\n"}


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

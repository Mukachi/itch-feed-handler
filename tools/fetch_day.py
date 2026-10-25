"""Fetch a Nasdaq TotalView-ITCH 5.0 sample day into data/ and check it (note 001, D2).

The files are Nasdaq's, published as sample data without stated terms
(note 001, Review): they are downloaded here, never committed or
redistributed. Each is checked against the size the server's listing gives
and the SHA-256 recorded at the first download.

Usage: python tools/fetch_day.py [day]        (default 01302020)
       python tools/fetch_day.py --check [day]  (check an existing file only)
"""

import hashlib
import pathlib
import sys
import urllib.request

BASE = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"
# day -> (bytes, SHA-256)
DAYS = {
    "01302020": (5_597_158_940, "8f5399155242f8d84338c36fdd78a47c0b0aa3df5707829b20c5158c258364c6"),
}
ROOT = pathlib.Path(__file__).resolve().parent.parent / "data"


def name(day):
    return f"{day}.NASDAQ_ITCH50.gz"


def check(path, size, digest):
    if path.stat().st_size != size:
        return f"size {path.stat().st_size:,}, expected {size:,}"
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 24), b""):
            h.update(block)
    return None if h.hexdigest() == digest else f"SHA-256 {h.hexdigest()}, expected {digest}"


def fetch(day):
    size, digest = DAYS[day]
    path = ROOT / name(day)
    ROOT.mkdir(exist_ok=True)
    if not path.exists() or path.stat().st_size != size:
        part = path.with_suffix(".gz.part")
        req = urllib.request.Request(BASE + name(day), headers={"User-Agent": "itch-feed-handler"})
        with urllib.request.urlopen(req) as r, part.open("wb") as f:
            done = 0
            for block in iter(lambda: r.read(1 << 22), b""):
                f.write(block)
                done += len(block)
                print(f"\r{done / size:6.1%}", end="", file=sys.stderr)
        print(file=sys.stderr)
        part.rename(path)
    return path


def main(argv):
    only_check = "--check" in argv
    args = [a for a in argv if a != "--check"]
    day = args[0] if args else "01302020"
    if day not in DAYS:
        print(f"unknown day {day}; known: {', '.join(DAYS)}", file=sys.stderr)
        return 2
    path = ROOT / name(day) if only_check else fetch(day)
    if not path.exists():
        print(f"{path}: missing (run without --check to fetch it)", file=sys.stderr)
        return 1
    problem = check(path, *DAYS[day])
    print(f"{path}: {problem or 'size and SHA-256 as recorded'}")
    return 1 if problem else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

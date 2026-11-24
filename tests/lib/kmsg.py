#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Kernel log assertions for the lab's tests.

Reads structured records from /dev/kmsg, which unlike `dmesg` text carries
each message's syslog level, and checks them after a unique marker that
the test wrote first. That keeps every test independent of whatever the
kernel logged before.

    kmsg.py mark                          write a marker, print its id
    kmsg.py show    MARK                  print messages after the marker
    kmsg.py expect  MARK PATTERN...       each regex must match, in order
    kmsg.py absent  MARK PATTERN          the regex must not match
    kmsg.py count   MARK PATTERN          print how many messages match
    kmsg.py level   MARK PATTERN LEVEL    the first match must have LEVEL
                                          (number or name, e.g. warning)
    kmsg.py clean   MARK                  fail on BUG/WARNING/Oops/... output

Exit status: 0 success, 1 assertion failed, 2 usage or I/O error.
Python 3.8+, standard library only.
"""
import os
import re
import sys
import uuid
from typing import Iterable, List, NamedTuple, Optional, Sequence, Tuple

LEVELS = {
    "emerg": 0, "alert": 1, "crit": 2, "err": 3, "error": 3,
    "warning": 4, "warn": 4, "notice": 5, "info": 6, "debug": 7,
}

# Signs that the kernel itself is unhappy, whatever module caused it.
PROBLEM_PATTERNS = [
    r"\bBUG:", r"\bWARNING:", r"\bOops\b", r"general protection fault",
    r"\bKASAN:", r"\bUBSAN:", r"\bKCSAN:", r"possible circular locking",
    r"\bINFO: task .* blocked", r"kernel BUG at", r"unreferenced object",
    r"Unable to handle kernel",
]


class Record(NamedTuple):
    level: int
    seq: int
    usec: int
    text: str


def parse_record(raw: str) -> Optional[Record]:
    """Parse one /dev/kmsg record: "pri,seq,usec,flags[,...];text".

    Continuation lines (starting with a space, e.g. KEY=value dictionary
    entries) are ignored. Returns None for anything malformed.
    """
    raw = raw.rstrip("\n")
    head, sep, text = raw.partition(";")
    if not sep:
        return None
    fields = head.split(",")
    if len(fields) < 3:
        return None
    try:
        pri, seq, usec = int(fields[0]), int(fields[1]), int(fields[2])
    except ValueError:
        return None
    # The first line holds the message; the rest are dictionary entries.
    text = text.split("\n", 1)[0]
    return Record(level=pri & 7, seq=seq, usec=usec, text=_unescape(text))


def _unescape(text: str) -> str:
    """/dev/kmsg escapes non-printable bytes as \\xNN."""
    return re.sub(r"\\x([0-9a-fA-F]{2})", lambda m: chr(int(m.group(1), 16)), text)


def read_kmsg(path: str = "/dev/kmsg") -> List[Record]:
    """Read every record currently buffered, without blocking."""
    records = []
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        while True:
            try:
                raw = os.read(fd, 8192)
            except BlockingIOError:
                break
            except BrokenPipeError:
                continue  # oldest records were overwritten; keep going
            if not raw:
                break
            rec = parse_record(raw.decode("utf-8", "replace"))
            if rec:
                records.append(rec)
    finally:
        os.close(fd)
    return records


def after_marker(records: Sequence[Record], marker: str) -> List[Record]:
    """Records strictly after the last record containing @marker."""
    idx = None
    for i, rec in enumerate(records):
        if marker in rec.text:
            idx = i
    if idx is None:
        raise LookupError(f"marker {marker!r} not found in kernel log")
    return list(records[idx + 1:])


def expect_in_order(records: Sequence[Record],
                    patterns: Iterable[str]) -> Tuple[bool, str]:
    """Each pattern must match some record after the previous match."""
    pos = 0
    for pat in patterns:
        rx = re.compile(pat)
        for i in range(pos, len(records)):
            if rx.search(records[i].text):
                pos = i + 1
                break
        else:
            return False, f"no match for {pat!r} after record {pos}"
    return True, "ok"


def count_matches(records: Sequence[Record], pattern: str) -> int:
    rx = re.compile(pattern)
    return sum(1 for r in records if rx.search(r.text))


def first_match(records: Sequence[Record], pattern: str) -> Optional[Record]:
    rx = re.compile(pattern)
    return next((r for r in records if rx.search(r.text)), None)


def find_problems(records: Sequence[Record]) -> List[Record]:
    rx = re.compile("|".join(PROBLEM_PATTERNS))
    return [r for r in records if rx.search(r.text)]


def parse_level(s: str) -> int:
    if s.isdigit() and 0 <= int(s) <= 7:
        return int(s)
    try:
        return LEVELS[s.lower()]
    except KeyError:
        raise ValueError(f"unknown log level {s!r}") from None


def write_marker(path: str = "/dev/kmsg") -> str:
    marker = f"lkl-test-mark-{uuid.uuid4().hex[:12]}"
    with open(path, "w") as f:
        f.write(f"<6>{marker}\n")
    return marker


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, args = argv[1], argv[2:]
    try:
        if cmd == "mark":
            print(write_marker())
            return 0
        if not args:
            raise ValueError("missing MARK")
        recs = after_marker(read_kmsg(), args[0])
        if cmd == "show":
            for r in recs:
                print(f"<{r.level}> {r.text}")
            return 0
        if cmd == "expect":
            ok, detail = expect_in_order(recs, args[1:])
            if not ok:
                print(f"kmsg expect failed: {detail}", file=sys.stderr)
            return 0 if ok else 1
        if cmd == "absent":
            n = count_matches(recs, args[1])
            if n:
                print(f"kmsg: {n} unexpected match(es) for {args[1]!r}",
                      file=sys.stderr)
            return 1 if n else 0
        if cmd == "count":
            print(count_matches(recs, args[1]))
            return 0
        if cmd == "level":
            rec = first_match(recs, args[1])
            want = parse_level(args[2])
            if rec is None:
                print(f"kmsg: no match for {args[1]!r}", file=sys.stderr)
                return 1
            if rec.level != want:
                print(f"kmsg: {args[1]!r} logged at level {rec.level}, "
                      f"expected {want}", file=sys.stderr)
                return 1
            return 0
        if cmd == "clean":
            bad = find_problems(recs)
            for r in bad:
                print(f"kmsg: kernel problem: {r.text}", file=sys.stderr)
            return 1 if bad else 0
    except (LookupError, ValueError, IndexError, OSError) as e:
        print(f"kmsg: {e}", file=sys.stderr)
        return 2
    print(f"kmsg: unknown command {cmd!r}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

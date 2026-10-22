"""``dktools testtime``: how long each section of one datakit test takes.

Runs ``BINARY test NAME`` and timestamps every ``test — SECTION`` line
the ctest.h harness prints.  A section lasts from its header to the next
header (the last one to the program's exit).  Prints the slowest
sections, so a slow suite shows which part to shrink or to mark slow.

Timestamps are taken when a line arrives: datakit-test line-buffers its
stdout, so every line, a suite's own printf headers included, arrives
when printed.  Besides ctest.h's "test — " headers, the headers suites
print themselves (SUITE_PREFIXES) open sections too; --prefix adds more.

The program runs in its own process group, bounded by ``--timeout``
(stopped, with its children, when it runs over).  Exits with 1 when the
test fails or times out, 0 otherwise.
"""

from __future__ import annotations

import re
import sys
import tempfile
import threading
import time
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from . import procs

#: The section header ctest.h's TEST and TEST_DESC macros print.
SECTION_PREFIX = "test — "

#: Headers suites print themselves (flex's "Test ...:", "Stress ...",
#: "FUZZ: ...", and "Testing ..."), recognized by default too.
SUITE_PREFIXES = ("Test ", "Testing ", "Stress ", "FUZZ: ")


@dataclass(frozen=True)
class Section:
    name: str
    seconds: float


def sections(
    lines: Iterable[tuple[float, str]],
    end: float,
    prefixes: tuple[str, ...] = (SECTION_PREFIX,),
) -> list[Section]:
    """Sections from (monotonic time, output line) pairs; a line starting
    with one of 'prefixes' opens a section, 'end' closes the last one.
    Lines before the first header belong to no section."""
    found: list[Section] = []
    current: tuple[str, float] | None = None
    for stamp, line in lines:
        prefix = next((p for p in prefixes if line.startswith(p)), None)
        if prefix is not None:
            if current is not None:
                found.append(Section(current[0], stamp - current[1]))
            current = (line[len(prefix) :].strip(), stamp)
    if current is not None:
        found.append(Section(current[0], end - current[1]))
    return found


_NUMBER = re.compile(r"\d+")


def groups(found: list[Section]) -> list[tuple[str, int, float]]:
    """(pattern, sections, seconds) for sections whose names differ only
    in their numbers (generated sections such as "N bytes max with M
    pairs"), slowest group first."""
    acc: dict[str, list[float]] = {}
    for s in found:
        key = _NUMBER.sub("#", s.name)
        entry = acc.setdefault(key, [0, 0.0])
        entry[0] += 1
        entry[1] += s.seconds
    return sorted(
        ((k, int(v[0]), v[1]) for k, v in acc.items()),
        key=lambda g: g[2],
        reverse=True,
    )


def render(found: list[Section], total: float, top: int) -> str:
    """The 'top' slowest section groups, then the 'top' slowest single
    sections, slowest first, with their share."""

    def share(seconds: float) -> float:
        return 100.0 * seconds / total if total > 0 else 0.0

    out = [f"{total:8.2f}s total, {len(found)} sections"]
    out.append("slowest groups (numbers in names masked as #):")
    for key, count, seconds in groups(found)[:top]:
        out.append(f"{seconds:8.2f}s {share(seconds):5.1f}%  x{count}  {key}")
    out.append("slowest sections:")
    for s in sorted(found, key=lambda s: s.seconds, reverse=True)[:top]:
        out.append(f"{s.seconds:8.2f}s {share(s.seconds):5.1f}%  {s.name}")
    return "\n".join(out)


def run(
    binary: Path,
    test: str,
    timeout: float,
    top: int,
    extra_prefixes: tuple[str, ...] = (),
) -> int:
    """Time one test's sections; 0 when it passes, 1 when it fails or
    runs past 'timeout' seconds.  'extra_prefixes' are more section
    header prefixes, for suites that print their own (such as "Test ")."""
    if not binary.exists():
        raise procs.ToolError(f"{binary}: no such test binary")
    stamped: list[tuple[float, str]] = []
    timed_out = threading.Event()
    start = time.monotonic()
    with tempfile.TemporaryFile() as err:
        with procs.streaming([str(binary), "test", test], err) as proc:

            def stop() -> None:
                timed_out.set()
                procs.kill_group(proc)

            watchdog = threading.Timer(timeout, stop)
            watchdog.daemon = True
            watchdog.start()
            try:
                assert proc.stdout is not None
                for raw in proc.stdout:
                    stamped.append(
                        (time.monotonic(), raw.decode("utf-8", errors="replace"))
                    )
            finally:
                watchdog.cancel()
    end = time.monotonic()
    found = sections(
        stamped, end, (SECTION_PREFIX, *SUITE_PREFIXES, *extra_prefixes)
    )
    print(render(found, end - start, top))
    if timed_out.is_set():
        print(f"testtime: {test} ran past {timeout:g}s; stopped", file=sys.stderr)
        return 1
    if proc.returncode != 0:
        print(f"testtime: {test} failed (exit {proc.returncode})", file=sys.stderr)
        return 1
    return 0

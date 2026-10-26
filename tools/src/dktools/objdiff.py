"""Machine code of named functions in a perfcompare run's two builds.

``dktools objdiff NAME SYMBOL [SYMBOL ...]`` disassembles each SYMBOL
from the ``datakit-test`` binaries that ``dktools perfcompare NAME``
built (``base`` and ``current``) and reports, per symbol:

- where it starts in each build and the start's offset within a 64-byte
  line (``@0x...+16``): a function whose code is identical can still
  time differently when its start moves against the fetch line;
- its instruction count in each build;
- ``identical`` when the instructions match once addresses are taken
  out, else ``differs`` followed by a unified diff of the two listings.

The perf builds link with LTO, so object files hold bitcode; the linked
binary is where the machine code is.  Addresses come out of the
comparison: a branch or call keeps only its ``<symbol+offset>``
annotation, and any hex literal of 0x10000 or more (a page address)
reads ``ADDR``.  Offsets into data that moved can still show as
``differs``; the diff says which lines.

Exit 0 when every symbol is identical, 1 when one differs, 2 when a
build or symbol is missing.
"""

from __future__ import annotations

import difflib
import re
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from . import procs
from .gitutil import ToolError

SIDES = ("base", "current")

#: Bytes in the line a function start is reported against.
FETCH_LINE = 64

_HEADER = re.compile(r"^([0-9a-fA-F]+) <(.+)>:\s*$")
_INSN = re.compile(r"^\s*([0-9a-fA-F]+):\s+(\S.*)$")
_TARGET = re.compile(r"0x[0-9a-fA-F]+ (<[^>]+>)")
_BIG_HEX = re.compile(r"0x[0-9a-fA-F]{5,}")


@dataclass(frozen=True)
class Listing:
    """One function's disassembly: start address and normalized lines."""

    symbol: str
    start: int
    lines: tuple[str, ...]

    @property
    def placement(self) -> str:
        return f"@{self.start:#x}+{self.start % FETCH_LINE}"


def normalize(insn: str) -> str:
    """An instruction's text with its addresses taken out."""
    text = _TARGET.sub(r"\1", insn)
    text = _BIG_HEX.sub("ADDR", text)
    return " ".join(text.split())


def parse(output: str, symbol: str) -> Listing | None:
    """The listing of `symbol` (with or without the Mach-O underscore)
    in llvm-objdump output, or None when it is absent."""
    wanted = {symbol, "_" + symbol}
    start: int | None = None
    lines: list[str] = []
    for raw in output.splitlines():
        header = _HEADER.match(raw)
        if header:
            if start is not None:
                break
            if header.group(2) in wanted:
                start = int(header.group(1), 16)
            continue
        if start is None:
            continue
        insn = _INSN.match(raw)
        if insn:
            lines.append(normalize(insn.group(2)))
        elif not raw.strip() and lines:
            break
    if start is None:
        return None
    return Listing(symbol, start, tuple(lines))


def binary(root: Path, name: str, side: str) -> Path:
    """The datakit-test that perfcompare NAME built for `side`."""
    found = sorted(
        (root / "perfcompare" / name / side).glob("build-*/src/datakit-test")
    )
    if len(found) != 1:
        what = "no" if not found else f"{len(found)}"
        raise ToolError(
            f"perfcompare {name}: {what} {side} datakit-test under "
            f"{root / 'perfcompare' / name / side} (run dktools perfcompare {name} first)"
        )
    return found[0]


def disassemble(objdump: str, program: Path, symbol: str) -> Listing:
    """`symbol`'s listing in `program`; ToolError when it is absent."""
    done = procs.run(
        [
            objdump,
            "-d",
            "--no-show-raw-insn",
            f"--disassemble-symbols=_{symbol},{symbol}",
            str(program),
        ],
        timeout=120.0,
    )
    if done.returncode != 0:
        err = done.stderr.decode("utf-8", "replace").strip()
        raise ToolError(f"{objdump}: {program}: {err}")
    listing = parse(done.stdout.decode("utf-8", "replace"), symbol)
    if listing is None or not listing.lines:
        raise ToolError(f"{program}: no function {symbol}")
    return listing


def compare(base: Listing, current: Listing) -> list[str]:
    """Report lines for one symbol: placement, sizes, verdict, diff."""
    same = base.lines == current.lines
    lines = [
        f"{base.symbol}: base {base.placement} {len(base.lines)} insns, "
        f"current {current.placement} {len(current.lines)} insns, "
        + ("identical" if same else "differs")
    ]
    if not same:
        lines.extend(
            "  " + line.rstrip("\n")
            for line in difflib.unified_diff(
                base.lines, current.lines, "base", "current", lineterm="", n=2
            )
        )
    return lines


def run(root: Path, name: str, symbols: Sequence[str], llvm_dir: Path) -> int:
    """Compare every symbol between the two builds and print the report."""
    objdump = str(llvm_dir / "llvm-objdump")
    programs = {side: binary(root, name, side) for side in SIDES}
    differs = False
    for symbol in symbols:
        base, current = (disassemble(objdump, programs[s], symbol) for s in SIDES)
        report = compare(base, current)
        differs = differs or base.lines != current.lines
        print("\n".join(report))
    return 1 if differs else 0

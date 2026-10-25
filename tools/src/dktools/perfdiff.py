"""Compare two perfRegression TSV outputs.

Usage::

    dktools perfdiff BASELINE.tsv CURRENT.tsv [--tolerance PCT]

Exits 0 if every row in CURRENT is within ``--tolerance`` percent of the
matching row in BASELINE (default 10%).  Exits 1 if any row regressed
by more than the tolerance, meaning ns_per_op grew or mb_per_sec fell
by more than that percentage, and prints the offending rows as TSV to
stdout (a "REGRESSION" header line, then one line per row).  Exits 2 on
any input error: a file that is missing, unreadable, a directory, or not
UTF-8; a malformed PERF row; a metric that is not a finite,
non-negative number; or a tolerance that is not.  Input errors print
one line to stderr, never a traceback.

A baseline metric of 0 (perfRegression prints 0.0 below its printed
precision, and for mb_per_sec of a zero-byte row) has no relative
change, so that metric is not compared; a row whose baseline metrics
are both 0 is listed on stderr as skipped.

Row format produced by perfRegression.c::

    PERF\\t<module>\\t<name>\\t<bytes>\\t<iters>\\t<ns_per_op>\\t<mb_per_sec>

Lines that do not start with "PERF\\t" are ignored; perfRegression
emits a comment header line for human readers.

The (module, name, bytes) triple is the row key.  iters is ignored when
matching rows, so a baseline regenerated with a different iteration
count still compares.  Baseline rows missing from CURRENT and rows new
in CURRENT are listed on stderr and do not affect the exit code.

The baseline is a local file, not part of the repository, and this
command never modifies it.  Generate or refresh it with a perf build
when an intentional performance change lands::

    ./build-perf/src/datakit-test test perfRegression > perf.baseline.tsv
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass
from pathlib import Path


class PerfDiffInputError(Exception):
    """A TSV file is missing or malformed (exit code 2)."""


@dataclass(frozen=True)
class PerfRow:
    module: str
    name: str
    bytes_: int
    iters: int
    ns_per_op: float
    mb_per_sec: float

    @property
    def key(self) -> tuple[str, str, int]:
        return (self.module, self.name, self.bytes_)


def parse_tsv(path: Path) -> dict[tuple[str, str, int], PerfRow]:
    """Parse the PERF rows of a perfRegression TSV, keyed by row key."""
    rows: dict[tuple[str, str, int], PerfRow] = {}
    try:
        with path.open(encoding="utf-8") as f:
            for line_no, raw in enumerate(f, start=1):
                line = raw.rstrip("\n")
                if not line.startswith("PERF\t"):
                    continue
                parts = line.split("\t")
                if len(parts) != 7:
                    raise PerfDiffInputError(
                        f"{path}:{line_no}: malformed PERF row "
                        f"(expected 7 tabs, got {len(parts)})"
                    )
                try:
                    row = PerfRow(
                        module=parts[1],
                        name=parts[2],
                        bytes_=int(parts[3]),
                        iters=int(parts[4]),
                        ns_per_op=float(parts[5]),
                        mb_per_sec=float(parts[6]),
                    )
                except ValueError as exc:
                    raise PerfDiffInputError(
                        f"{path}:{line_no}: parse error: {exc}"
                    ) from exc
                for label, value, text in (
                    ("ns_per_op", row.ns_per_op, parts[5]),
                    ("mb_per_sec", row.mb_per_sec, parts[6]),
                ):
                    if not math.isfinite(value) or value < 0:
                        raise PerfDiffInputError(
                            f"{path}:{line_no}: {label} must be a finite, "
                            f"non-negative number (got {text!r})"
                        )
                rows[row.key] = row
    except FileNotFoundError as exc:
        raise PerfDiffInputError(f"{path}: file not found") from exc
    except UnicodeDecodeError as exc:
        raise PerfDiffInputError(f"{path}: not UTF-8 text: {exc}") from exc
    except OSError as exc:
        # A directory, a permission error, an I/O error: all input
        # errors, reported without a traceback.
        raise PerfDiffInputError(f"{path}: cannot read: {exc.strerror or exc}") from exc
    return rows


def comparable(base: PerfRow) -> bool:
    """True iff `base` has at least one metric a ratio can be taken of.

    perfRegression prints 0.0 when a metric rounds below its printed
    precision or has no meaningful value (mb_per_sec for a zero-byte
    row), and a zero baseline has no relative change."""
    return base.ns_per_op > 0 or base.mb_per_sec > 0


def regressed(base: PerfRow, cur: PerfRow, tolerance_pct: float) -> bool:
    """True iff `cur` is more than `tolerance_pct` worse than `base`.

    Both ns_per_op (lower is better) and mb_per_sec (higher is better)
    are compared, and either moving adversely past the tolerance counts
    as a regression, so a drift visible in only one metric is still
    caught.  A metric whose baseline is 0 has no relative change and is
    not compared; the other metric still is."""
    tol = tolerance_pct / 100.0
    if base.ns_per_op > 0:
        ns_ratio = (cur.ns_per_op - base.ns_per_op) / base.ns_per_op
        if ns_ratio > tol:
            return True
    if base.mb_per_sec > 0:
        mb_ratio = (base.mb_per_sec - cur.mb_per_sec) / base.mb_per_sec
        if mb_ratio > tol:
            return True
    return False


def run(baseline: Path, current: Path, tolerance_pct: float) -> int:
    """Compare `current` against `baseline`; return the exit code."""
    if not math.isfinite(tolerance_pct) or tolerance_pct < 0:
        print(
            f"--tolerance must be a finite, non-negative percent (got {tolerance_pct})",
            file=sys.stderr,
        )
        return 2

    try:
        base = parse_tsv(baseline)
        cur = parse_tsv(current)
    except PerfDiffInputError as exc:
        print(exc, file=sys.stderr)
        return 2

    missing = [k for k in base if k not in cur]
    new = [k for k in cur if k not in base]
    incomparable = [k for k, row in base.items() if k in cur and not comparable(row)]
    regressions = [
        (base_row, cur[key])
        for key, base_row in base.items()
        if key in cur and regressed(base_row, cur[key], tolerance_pct)
    ]

    if incomparable:
        print(
            f"# {len(incomparable)} row(s) not compared "
            "(baseline ns_per_op and mb_per_sec are both 0):",
            file=sys.stderr,
        )
        for k in incomparable:
            print("# skipped\t" + "\t".join(map(str, k)), file=sys.stderr)

    if missing:
        print(
            f"# {len(missing)} baseline row(s) missing from current "
            "(was a benchmark deleted?):",
            file=sys.stderr,
        )
        for k in missing:
            print("# missing\t" + "\t".join(map(str, k)), file=sys.stderr)

    if new:
        print(
            f"# {len(new)} new row(s) in current not in baseline "
            "(regenerate baseline):",
            file=sys.stderr,
        )
        for k in new:
            print("# new    \t" + "\t".join(map(str, k)), file=sys.stderr)

    if regressions:
        print("REGRESSION\tmodule\tname\tbytes\tbase_ns\tcur_ns\tbase_mb\tcur_mb")
        for base_row, cur_row in regressions:
            print(
                "REGRESSION\t"
                f"{base_row.module}\t{base_row.name}\t{base_row.bytes_}\t"
                f"{base_row.ns_per_op:.1f}\t{cur_row.ns_per_op:.1f}\t"
                f"{base_row.mb_per_sec:.1f}\t{cur_row.mb_per_sec:.1f}"
            )
        return 1
    return 0

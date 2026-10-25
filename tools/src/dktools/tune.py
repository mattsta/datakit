"""Measure each tiered family's speed and memory against its size limit on
this machine, against the default limit, and say whether a different
limit is a clear win.

Usage::

    dktools tune NAME [--families F[,F...]] [--mix [NAME:]K=N[,K=N...]]...
        [--baseline LIMIT] [--runs N] [--compiler C] [--root DIR] [-j N]

HEAD plus every changed tracked path (as ``verify --all-changed``) is
built under ``ROOT/tune/NAME`` with the compiler's perf preset (gcc15:
its release preset), ``datakit-test`` only.  ``datakit-test test
perfRegression`` then runs ``--runs`` times with ``DK_PERF_MODULES``
naming the families' size-limit sweeps (``<family>Limits``: every
operation at every limit from 256 bytes to 8 KB, rows named
``<op><limit>``).

Everything is read against the baseline, the limit the plain
constructors use today (``--baseline``, default 2048, marked ``*``).  Per
family and limit the table shows each operation's median ns/op and its
change against the baseline, the filled container's bytes and their
change, and per workload mix the mix's mean ns per operation against the
baseline's: 0.85 means 15% faster, with ``[lowest-highest]`` over the
runs (each run compared with itself).  A spread line gives each
operation's run-to-run noise.

A mix says how often each operation runs: ``equal`` (each as often),
``read80`` (80% reads, 20% writes), ``write80`` (the reverse), plus any
``--mix`` (repeatable; ``lookups:exists=90,insert=10`` names one, shares
go by class, ``read``/``write``, or by operation, and an operation's own
share wins over its class's).

Per mix the result is "keep" unless a limit is a clear win: ``SPEED_WIN``
(15%) faster for the mix in every run, or ``BYTES_WIN`` (5%) fewer bytes
while less than ``SLOWER`` (15%) slower for the mix in every run; and
never one whose mix, or any one operation the mix runs, is 15% or more
slower.  Among wins the one holding the data in the fewest bytes is
named, with the call that applies it; a "keep" names the nearest limit
and its ratio.  The machine's cache sizes are printed too.  The report is
also written to ``result.txt`` and the analysis to ``result.json``.
"""

from __future__ import annotations

import json
import math
import re
import statistics
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, gitutil, perfcompare, perfdiff, procs, steps, toolchain
from .gitutil import ToolError

FAMILIES = ("multimap", "multiOrderedSet", "multilist")
#: The limit the plain constructors use until changed (DK_TIER_LIMIT_COMPILED
#: in dkTierLimit.h): the baseline every result is read against
DEFAULT_BASELINE = 2048
#: A limit wins on speed when its mix runs this much faster than at the
#: baseline in every run
SPEED_WIN = 0.15
#: A limit wins on memory when it holds the data in this much fewer bytes
#: (and its mix stays under SLOWER in every run)
BYTES_WIN = 0.05
#: No win may make the mix, or one operation the mix runs, this much slower
SLOWER = 0.15
#: Slack for the threshold comparisons, so a ratio that is the threshold
#: in exact arithmetic counts as reaching it
_EPS = 1e-9
#: The dkTierFamily whose process default each family's plain constructor
#: reads (dkTierLimit.h)
DEFAULT_SETTERS = {
    "multimap": "DK_TIER_MULTIMAP",
    "multiOrderedSet": "DK_TIER_MULTIORDEREDSET",
    "multilist": "DK_TIER_MULTILIST",
}

#: Each family's sweep operations, as reads or writes
OP_CLASS = {
    "multimap": {"insert": "write", "exists": "read", "delete": "write"},
    "multiOrderedSet": {
        "add": "write",
        "getScore": "read",
        "getRank": "read",
        "remove": "write",
    },
    "multilist": {
        "pushTail": "write",
        "index": "read",
        "delete": "write",
        "popHead": "write",
    },
}
CLASSES = ("read", "write")

_ROW = re.compile(r"^(?P<op>[A-Za-z]+?)(?P<limit>\d+)$")


@dataclass(frozen=True)
class Mix:
    """How often each operation runs: relative shares by class (``read``,
    ``write``) or by operation name.  No shares: every operation equally."""

    name: str
    shares: dict[str, float] = field(default_factory=dict)

    def weights(self, family: str, ops: list[str]) -> dict[str, float]:
        """Per operation of 'ops', its fraction of the mix (summing to 1),
        or {} when the mix runs none of them.  A class's share is split
        evenly over its operations that have no share of their own."""
        if not self.shares:
            return {op: 1 / len(ops) for op in ops} if ops else {}
        classes = OP_CLASS.get(family, {})
        raw = {op: self.shares[op] for op in ops if op in self.shares}
        for cls in CLASSES:
            members = [op for op in ops if classes.get(op) == cls and op not in raw]
            for op in members:
                raw[op] = self.shares.get(cls, 0.0) / len(members)
        total = sum(raw.values())
        return {op: w / total for op, w in raw.items() if w > 0} if total else {}


MIXES = (
    Mix("equal"),
    Mix("read80", {"read": 80, "write": 20}),
    Mix("write80", {"read": 20, "write": 80}),
)


_MIX_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")


def parse_mix(text: str | None, name: str = "custom") -> Mix | None:
    """One --mix: '[NAME:]key=share,...', keys a class or a sweep
    operation; 'name' when the text names none"""
    if text is None:
        return None
    head, colon, rest = text.partition(":")
    if colon:
        if not _MIX_NAME.match(head):
            raise ToolError(f"--mix: {head!r} is not a mix name (letters, digits, - _)")
        name, text = head, rest
    known = set(CLASSES) | {op for ops in OP_CLASS.values() for op in ops}
    shares: dict[str, float] = {}
    for part in [p for p in text.split(",") if p]:
        key, sep, value = part.partition("=")
        if not sep or key not in known:
            raise ToolError(
                f"--mix: {part!r} is not KEY=SHARE with KEY one of "
                f"{', '.join(sorted(known))}"
            )
        try:
            share = float(value)
        except ValueError as exc:
            raise ToolError(f"--mix: share {value!r} is not a number") from exc
        if share < 0 or not math.isfinite(share):
            raise ToolError(f"--mix: share {value!r} must be 0 or more")
        shares[key] = share
    if not any(shares.values()):
        raise ToolError("--mix: give at least one share above 0")
    return Mix(name, shares)


def parse_mixes(texts: list[str] | None) -> tuple[Mix, ...]:
    """The built-in mixes, then one per --mix (unnamed ones are custom,
    custom2, ...); names must not repeat"""
    out = list(MIXES)
    unnamed = 0
    for text in texts or []:
        if ":" not in text:
            unnamed += 1
        mix = parse_mix(text, "custom" if unnamed < 2 else f"custom{unnamed}")
        if mix.name in {m.name for m in out}:
            raise ToolError(f"--mix: a mix named {mix.name!r} is already defined")
        out.append(mix)
    return tuple(out)


@dataclass(frozen=True)
class Spread:
    """A value's median over the runs, and its lowest and highest"""

    median: float
    low: float
    high: float

    @classmethod
    def of(cls, values: list[float]) -> Spread:
        return cls(statistics.median(values), min(values), max(values))


@dataclass(frozen=True)
class Decision:
    """What a mix's numbers say about the baseline"""

    mix: str
    baseline: int
    #: "keep", or the win: "faster" or "smaller"
    why: str
    #: the limit to use (the baseline when keeping)
    limit: int
    #: the limit's mix time against the baseline's
    versus: Spread
    #: when keeping: the fastest other limit for the mix, and its ratio
    nearest: tuple[int, Spread] | None = None
    #: when keeping: why the nearest limit is no win
    blocked: str = ""
    #: when keeping: the densest limit holding fewer bytes than the
    #: baseline whose median mix time is under SLOWER (and no operation
    #: SLOWER) slower, and its ratio: the memory side of the choice
    denser: tuple[int, Spread] | None = None

    @property
    def change(self) -> bool:
        return self.why != "keep"


@dataclass
class TuneOptions:
    name: str
    families: list[str] = field(default_factory=lambda: list(FAMILIES))
    mixes: tuple[Mix, ...] = MIXES
    baseline: int = DEFAULT_BASELINE
    runs: int = 5
    compiler: toolchain.Compiler = field(
        default_factory=lambda: toolchain.resolve_compiler(toolchain.DEFAULT_COMPILER)
    )
    root: Path | None = None
    jobs: int | None = None
    #: report on the runs an earlier tune of this name left, without
    #: building or measuring
    reuse: bool = False


@dataclass
class Sweep:
    """One family's measurements: samples[limit][op] holds one ns/op per
    run, in run order (run r of every cell is the same run), and
    bytes[limit] the filled container's size."""

    family: str = ""
    samples: dict[int, dict[str, list[float]]] = field(default_factory=dict)
    bytes: dict[int, int] = field(default_factory=dict)

    @classmethod
    def of_medians(
        cls,
        ns: dict[int, dict[str, float]],
        bytes_: dict[int, int] | None = None,
        family: str = "",
    ) -> Sweep:
        """A one-run sweep from ns[limit][op]"""
        return cls(
            family,
            {limit: {op: [v] for op, v in per.items()} for limit, per in ns.items()},
            dict(bytes_ or {}),
        )

    @property
    def ns(self) -> dict[int, dict[str, float]]:
        """Per limit and operation, the median ns/op over the runs"""
        return {
            limit: {op: statistics.median(v) for op, v in per.items()}
            for limit, per in self.samples.items()
        }

    def ops(self) -> list[str]:
        seen: list[str] = []
        for per in self.samples.values():
            for op in per:
                if op not in seen:
                    seen.append(op)
        return seen

    def common_ops(self) -> list[str]:
        """The operations every limit measured"""
        return [
            op for op in self.ops() if all(op in per for per in self.samples.values())
        ]

    def runs(self) -> int:
        """The runs every common cell has"""
        counts = [
            len(per[op]) for per in self.samples.values() for op in self.common_ops()
        ]
        return min(counts) if counts else 0

    def op_change(self, baseline: int) -> dict[int, dict[str, float]]:
        """Per limit and operation, its median against the baseline's
        (0.06: 6% slower)"""
        ns = self.ns
        base = ns.get(baseline, {})
        return {
            limit: {op: per[op] / base[op] - 1 for op in per if base.get(op, 0) > 0}
            for limit, per in ns.items()
        }

    def noise(self) -> dict[str, float]:
        """Per operation, its run-to-run noise: half the range of its runs
        over their median, the median of that over the limits"""
        out = {}
        for op in self.common_ops():
            halves = []
            for per in self.samples.values():
                values = per[op]
                mid = statistics.median(values)
                if len(values) > 1 and mid > 0:
                    halves.append((max(values) - min(values)) / 2 / mid)
            if halves:
                out[op] = statistics.median(halves)
        return out

    def versus(self, mix: Mix, baseline: int) -> dict[int, Spread]:
        """Per limit, the mix's mean ns per operation there against the
        baseline's, each run against itself ({} when the mix runs none of
        the common operations)"""
        weights = mix.weights(self.family, self.common_ops())
        if not weights or baseline not in self.samples:
            return {}

        def mean(limit: int, r: int) -> float:
            return sum(w * self.samples[limit][op][r] for op, w in weights.items())

        out = {}
        for limit in self.samples:
            ratios = [
                mean(limit, r) / mean(baseline, r)
                for r in range(self.runs())
                if mean(baseline, r) > 0
            ]
            if ratios:
                out[limit] = Spread.of(ratios)
        return out

    def decide(self, mix: Mix, baseline: int) -> Decision | None:
        """Keep the baseline unless a limit is a clear win for 'mix'
        (None when the mix runs none of these operations)"""
        vs = self.versus(mix, baseline)
        if not vs:
            return None
        ran = mix.weights(self.family, self.common_ops())
        change = self.op_change(baseline)
        base_bytes = self.bytes.get(baseline)

        def no_op_slower(limit: int) -> bool:
            return all(change[limit].get(op, 0.0) < SLOWER - _EPS for op in ran)

        others = [limit for limit in vs if limit != baseline]
        faster = [
            limit
            for limit in others
            if vs[limit].high <= 1 - SPEED_WIN + _EPS and no_op_slower(limit)
        ]
        smaller = [
            limit
            for limit in others
            if base_bytes
            and limit in self.bytes
            and self.bytes[limit] <= (1 - BYTES_WIN) * base_bytes + _EPS
            and vs[limit].high < 1 + SLOWER - _EPS
            and no_op_slower(limit)
        ]

        def densest(limits: list[int]) -> int:
            return min(
                limits, key=lambda x: (self.bytes.get(x, math.inf), vs[x].median)
            )

        if faster:
            pick = densest(faster)
            return Decision(mix.name, baseline, "faster", pick, vs[pick])
        if smaller:
            pick = densest(smaller)
            return Decision(mix.name, baseline, "smaller", pick, vs[pick])
        nearest = min(others, key=lambda x: vs[x].median) if others else None
        if nearest is None:
            return Decision(mix.name, baseline, "keep", baseline, vs[baseline])
        near = vs[nearest]
        reasons = []
        if near.median >= 1:
            reasons.append("no limit is faster")
        elif near.high > 1 - SPEED_WIN + _EPS:
            reasons.append(
                f"its slowest run is {near.high:.3f}; a win needs "
                f"{1 - SPEED_WIN:.2f} or less"
            )
        slow = [
            f"{op} {change[nearest][op] * 100:+.0f}%"
            for op in ran
            if change[nearest].get(op, 0.0) >= SLOWER - _EPS
        ]
        if slow:
            reasons.append(f"{', '.join(slow)} there")
        close = [
            limit
            for limit in others
            if base_bytes
            and self.bytes.get(limit, base_bytes) < base_bytes
            and vs[limit].median < 1 + SLOWER - _EPS
            and no_op_slower(limit)
        ]
        denser = densest(close) if close else None
        return Decision(
            mix.name,
            baseline,
            "keep",
            baseline,
            vs[baseline],
            (nearest, near),
            "; ".join(reasons),
            (denser, vs[denser]) if denser is not None else None,
        )


def sweeps(runs: list[dict[perfcompare.Key, perfdiff.PerfRow]]) -> dict[str, Sweep]:
    """The families' sweeps out of perfRegression runs (rows of
    ``<family>Limits`` modules named ``<op><limit>``), in the order the
    first run printed them.  Rows match across runs by module and name (a
    row's byte count can differ between runs: a hashed container's size
    does); only rows every run has are kept, so sample r of every cell is
    run r, and a limit's bytes are the median over the runs."""
    if not runs:
        return {}
    by_name = [{(r.module, r.name): r for r in rows.values()} for rows in runs]
    out: dict[str, Sweep] = {}
    for module, name in by_name[0]:
        if not module.endswith("Limits") or any(
            (module, name) not in b for b in by_name
        ):
            continue
        m = _ROW.match(name)
        if not m:
            continue
        family = module[: -len("Limits")]
        sweep = out.setdefault(family, Sweep(family=family))
        limit = int(m.group("limit"))
        rows = [b[(module, name)] for b in by_name]
        sweep.samples.setdefault(limit, {})[m.group("op")] = [r.ns_per_op for r in rows]
        sizes = [r.bytes_ for r in rows if r.bytes_]
        if sizes:
            sweep.bytes[limit] = round(statistics.median(sizes))
    return out


# -- the machine's caches ----------------------------------------------

_SYSCTL_KEYS = (
    ("L1d (performance cores)", "hw.perflevel0.l1dcachesize"),
    ("L2 (performance cores)", "hw.perflevel0.l2cachesize"),
    ("L1d", "hw.l1dcachesize"),
    ("L2", "hw.l2cachesize"),
    ("L3", "hw.l3cachesize"),
)

_SIZE = re.compile(r"^(?P<n>\d+)(?P<unit>[KMG]?)$")


def _bytes(text: str) -> int | None:
    m = _SIZE.match(text.strip().upper())
    if not m:
        return None
    scale = {"": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}[m.group("unit")]
    return int(m.group("n")) * scale


def cache_sizes(
    sysctl: Callable[[str], str | None] | None = None,
    sys_root: Path = Path("/sys/devices/system/cpu/cpu0/cache"),
) -> list[tuple[str, int]]:
    """(label, bytes) for each cache level the system reports: Linux
    sysfs when present, else sysctl (macOS).  Empty when neither says."""
    found: list[tuple[str, int]] = []
    if sys_root.is_dir():
        for index in sorted(sys_root.glob("index*")):
            try:
                level = (index / "level").read_text().strip()
                kind = (index / "type").read_text().strip()
                size = _bytes((index / "size").read_text())
            except OSError:
                continue
            if size and kind != "Instruction":
                label = f"L{level}" + ("d" if kind == "Data" else "")
                found.append((label, size))
        if found:
            return found

    def default_sysctl(key: str) -> str | None:
        try:
            done = procs.run(["sysctl", "-n", key], timeout=10)
        except (procs.ToolError, procs.TimedOut):
            return None
        return done.stdout.decode().strip() if done.returncode == 0 else None

    read = sysctl or default_sysctl
    for label, key in _SYSCTL_KEYS:
        value = read(key)
        if value and value.isdigit() and int(value) > 0:
            found.append((label, int(value)))
    return found


# -- report --------------------------------------------------------------


def _human(n: int) -> str:
    for unit, scale in (("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= scale and n % scale == 0:
            return f"{n // scale} {unit}"
    return f"{n} B"


def apply_text(family: str, limit: int) -> str:
    """The C that makes ``limit`` the family's limit."""
    return f"dkTierSetDefaultLimit({DEFAULT_SETTERS[family]}, FLEX_CAP_LEVEL_{limit});"


def _mix_text(mix: Mix) -> str:
    if not mix.shares:
        return "every operation equally often"
    total = sum(mix.shares.values())
    return ", ".join(
        f"{key} {share / total:.0%}" for key, share in mix.shares.items() if share
    )


def _pct(change: float) -> str:
    return f"{change * 100:+.0f}%"


def _ratio(s: Spread, runs: int) -> str:
    return (
        f"{s.median:.2f}" if runs < 2 else f"{s.median:.2f} [{s.low:.2f}-{s.high:.2f}]"
    )


def _table(header: list[str], rows: list[list[tuple[str, str]]]) -> list[str]:
    """A table of (value, change) cells, change possibly empty: each
    column's values and changes right-aligned to its widest, so columns
    line up whatever a value or change holds, the header over each"""
    columns = len(header)
    vw = [max(len(r[c][0]) for r in rows) if rows else 0 for c in range(columns)]
    cw = [max(len(r[c][1]) for r in rows) if rows else 0 for c in range(columns)]
    width = [
        max(len(header[c]), vw[c] + (cw[c] + 1 if cw[c] else 0)) for c in range(columns)
    ]

    def cell(c: int, value: str, chg: str) -> str:
        text = value.rjust(vw[c]) + (" " + chg.rjust(cw[c]) if cw[c] else "")
        return text.rjust(width[c])

    out = ["  " + "  ".join(h.rjust(width[c]) for c, h in enumerate(header))]
    out.extend(
        ("  " + "  ".join(cell(c, *r[c]) for c in range(columns))).rstrip()
        for r in rows
    )
    return out


def legend(mixes: tuple[Mix, ...], baseline: int, runs: int) -> list[str]:
    """How to read the report, said once at its top"""
    lines = [
        f"reading   * marks the baseline, limit {baseline} (the default the plain "
        "constructors use)",
        "          an operation: median ns/op, then its change against the baseline",
        "          bytes: the filled container, then its change against the baseline",
        "          a mix: its mean ns per operation against the baseline's (0.85 = 15% "
        "faster), [lowest-highest] over the runs",
        f"          change only for a clear win: {SPEED_WIN:.0%} faster for the mix in "
        f"every run, or {BYTES_WIN:.0%} fewer bytes",
        f"          while under {SLOWER:.0%} slower in every run; never with one "
        f"operation {SLOWER:.0%} or more slower",
    ]
    if runs < 3:
        lines.append(
            f"          only {_runs(runs)}: the ranges and "
            "every-run checks are rough (use --runs 5 or more)"
        )
    lines.append(f"mixes     {mixes[0].name:<8} {_mix_text(mixes[0])}")
    lines.extend(f"          {mix.name:<8} {_mix_text(mix)}" for mix in mixes[1:])
    return lines


def decision_text(family: str, sweep: Sweep, d: Decision) -> str:
    runs = sweep.runs()
    if d.why == "keep":
        if not d.nearest:
            return f"keep {d.baseline}: no other limit measured"
        why = f": {d.blocked}" if d.blocked else ""
        denser = ""
        if d.denser:
            limit, s = d.denser
            shrink = 1 - sweep.bytes[limit] / sweep.bytes[d.baseline]
            denser = (
                f"; denser: {limit}, {shrink:.1%} fewer bytes at {_ratio(s, runs)} "
                f"(a memory win needs {BYTES_WIN:.0%})"
            )
        return (
            f"keep {d.baseline}: nearest is {d.nearest[0]} at "
            f"{_ratio(d.nearest[1], runs)}{why}{denser}"
        )
    if d.why == "faster":
        return (
            f"change to {d.limit}: {1 - d.versus.median:.0%} faster "
            f"({_ratio(d.versus, runs)}); apply: {apply_text(family, d.limit)}"
        )
    shrink = 1 - sweep.bytes[d.limit] / sweep.bytes[d.baseline]
    return (
        f"change to {d.limit}: {shrink:.1%} fewer bytes at "
        f"{_ratio(d.versus, runs)}; apply: {apply_text(family, d.limit)}"
    )


def render(
    found: dict[str, Sweep],
    caches: list[tuple[str, int]],
    mixes: tuple[Mix, ...] = MIXES,
    baseline: int = DEFAULT_BASELINE,
) -> list[str]:
    lines = []
    if caches:
        lines.append(
            "caches    " + ", ".join(f"{label} {_human(n)}" for label, n in caches)
        )
    runs = min((s.runs() for s in found.values()), default=0)
    lines.extend(legend(mixes, baseline, runs))
    for family, sweep in found.items():
        ops = sweep.ops()
        classes = OP_CLASS.get(family, {})
        ns = sweep.ns
        change = sweep.op_change(baseline)
        versus = {mix.name: sweep.versus(mix, baseline) for mix in mixes}
        reads = [op for op in ops if classes.get(op) == "read"]
        writes = [op for op in ops if classes.get(op) == "write"]
        lines.append("")
        lines.append(
            f"{family} (reads: {', '.join(reads) or '-'}; writes: "
            f"{', '.join(writes) or '-'})"
        )
        noise = sweep.noise()
        if noise:
            lines.append(
                "  run-to-run spread: "
                + ", ".join(f"{op} ±{noise[op]:.0%}" for op in ops if op in noise)
            )
        base_bytes = sweep.bytes.get(baseline)
        many = sweep.runs() >= 2
        rows: list[list[tuple[str, str]]] = []
        for limit in sorted(ns):
            per = ns[limit]
            row = [(f"{limit}{'*' if limit == baseline else ' '}", "")]
            for op in ops:
                if op not in per:
                    row.append(("-", ""))
                elif limit == baseline or op not in change[limit]:
                    row.append((f"{per[op]:.1f}", ""))
                else:
                    row.append((f"{per[op]:.1f}", _pct(change[limit][op])))
            nbytes = sweep.bytes.get(limit)
            if nbytes is None:
                row.append(("-", ""))
            elif limit == baseline or not base_bytes:
                row.append((str(nbytes), ""))
            else:
                row.append((str(nbytes), f"{(nbytes / base_bytes - 1) * 100:+.1f}%"))
            for mix in mixes:
                s = versus[mix.name].get(limit)
                if s is None:
                    row.append(("-", ""))
                elif limit == baseline:
                    row.append(("1.00", ""))
                else:
                    row.append(
                        (
                            f"{s.median:.2f}",
                            f"[{s.low:.2f}-{s.high:.2f}]" if many else "",
                        )
                    )
            rows.append(row)
        lines.extend(_table(["limit", *ops, "bytes", *(m.name for m in mixes)], rows))
        for mix in mixes:
            d = sweep.decide(mix, baseline)
            text = (
                decision_text(family, sweep, d)
                if d
                else "runs none of these operations"
            )
            lines.append(f"  {mix.name:<8} {text}")
    return lines


def _spread_json(s: Spread) -> dict[str, float]:
    return {
        "median": round(s.median, 4),
        "low": round(s.low, 4),
        "high": round(s.high, 4),
    }


def analysis(
    found: dict[str, Sweep],
    mixes: tuple[Mix, ...] = MIXES,
    baseline: int = DEFAULT_BASELINE,
) -> dict:
    """Everything the report says, as data: per family the noise, each
    limit's bytes and per-operation change, and per mix the decision and
    every limit's ratio against the baseline"""
    families: dict[str, dict] = {}
    for family, sweep in found.items():
        per_mix = {}
        for mix in mixes:
            d = sweep.decide(mix, baseline)
            if d is None:
                continue
            entry = {
                "shares": dict(mix.shares),
                "action": "change" if d.change else "keep",
                "why": d.why,
                "limit": d.limit,
                "versus": {
                    str(k): _spread_json(v)
                    for k, v in sweep.versus(mix, baseline).items()
                },
            }
            if d.change:
                entry["apply"] = apply_text(family, d.limit)
            if d.nearest:
                entry["nearest"] = d.nearest[0]
                entry["blocked"] = d.blocked
            if d.denser:
                entry["denser"] = d.denser[0]
            per_mix[mix.name] = entry
        families[family] = {
            "runs": sweep.runs(),
            "noise": {op: round(v, 4) for op, v in sweep.noise().items()},
            "bytes": {str(k): v for k, v in sweep.bytes.items()},
            "change": {
                str(k): {op: round(v, 4) for op, v in per.items()}
                for k, per in sweep.op_change(baseline).items()
            },
            "mixes": per_mix,
        }
    return {"baseline": baseline, "families": families}


# -- run -----------------------------------------------------------------


def parse_families(text: str | None) -> list[str]:
    if text is None:
        return list(FAMILIES)
    names = [n for n in text.split(",") if n]
    for name in names:
        if name not in FAMILIES:
            raise ToolError(f"--families: {name!r} is not one of {', '.join(FAMILIES)}")
    if not names:
        raise ToolError("--families: name at least one family")
    return names


def check_baseline(found: dict[str, Sweep], baseline: int) -> None:
    """ToolError unless every family measured the baseline limit"""
    for family, sweep in found.items():
        if baseline not in sweep.samples:
            raise ToolError(
                f"--baseline {baseline}: {family} measured only "
                f"{', '.join(map(str, sorted(sweep.samples)))}"
            )


_RUN_FILE = re.compile(r"^run-(\d+)\.tsv$")


def _runs(n: int) -> str:
    return f"{n} run{'' if n == 1 else 's'}"


def load_runs(work: Path) -> list[dict[perfcompare.Key, perfdiff.PerfRow]]:
    """The run-N.tsv files a measurement left in 'work', in run order"""
    files = sorted(
        (int(m.group(1)), p)
        for p in work.glob("run-*.tsv")
        if (m := _RUN_FILE.match(p.name))
    )
    if not files:
        raise ToolError(f"--reuse: {work} holds no run-N.tsv from an earlier tune")
    try:
        return [perfdiff.parse_tsv(p) for _, p in files]
    except perfdiff.PerfDiffInputError as exc:
        raise ToolError(str(exc)) from exc


def report(
    work: Path,
    title: str,
    runs: list[dict[perfcompare.Key, perfdiff.PerfRow]],
    opts: TuneOptions,
) -> int:
    """Analyse 'runs', print the report and write result.txt/.json"""
    found = {f: s for f, s in sweeps(runs).items() if f in opts.families}
    missing = [f for f in opts.families if f not in found]
    if missing:
        raise ToolError(f"no sweep rows for {', '.join(missing)}")
    check_baseline(found, opts.baseline)
    body = render(found, cache_sizes(), opts.mixes, opts.baseline)
    text = "\n".join([title, *body]) + "\n"
    (work / "result.txt").write_text(text)
    (work / "result.json").write_text(
        json.dumps(analysis(found, opts.mixes, opts.baseline), indent=2) + "\n"
    )
    sys.stdout.write(text)
    return 0


def run(repo: Path, opts: TuneOptions) -> int:
    """Build, sweep, report (or, with opts.reuse, report on an earlier
    measurement of the same name); the exit code (ToolError: input)."""
    name = gitutil.check_name(opts.name)
    if opts.baseline < 1:
        raise ToolError(f"--baseline must be a limit in bytes (got {opts.baseline})")
    if opts.runs < 1:
        raise ToolError(f"--runs must be at least 1 (got {opts.runs})")
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    root = gitutil.scratch_root(opts.root, repo).resolve()

    if opts.reuse:
        work = root / "tune" / name
        runs = load_runs(work)
        try:
            meta = json.loads((work / "meta.json").read_text())
        except (OSError, ValueError):
            meta = {}
        title = (
            f"tune {name}: {meta.get('config', 'unknown config')}, {_runs(len(runs))} "
            f"reused from {work}, medians of ns/op, tree {meta.get('tree', '?')[:10]}"
        )
        return report(work, title, runs, opts)

    config = toolchain.build_config(
        opts.compiler, perfcompare.perf_preset(opts.compiler)
    )
    size = procs.sizing(opts.jobs)

    head = gitutil.git_text(repo, "rev-parse", "HEAD")
    with tempfile.TemporaryDirectory(prefix="dktools-tune-") as tmp:
        tree = gitutil.build_tree(
            repo, Path(tmp) / "index", [(p, None) for p in gitutil.changed_paths(repo)]
        )
    config.compiler.check()

    work = buildstore.fresh_build(repo, root, "tune", name, head, "tune")
    (work / "meta.json").write_text(
        json.dumps({"config": config.label, "tree": tree}, indent=2) + "\n"
    )
    side = perfcompare.Side("tune", tree, work / "src", config)
    gitutil.export_tree(repo, tree, side.src)
    config.check_presets_file(side.src)
    step = perfcompare.build_side(side, work, size)
    if not step.ok:
        sys.stdout.write(
            f"tune {name}: FAIL ({step.name} {step.describe_failure()}, see {step.log})\n"
        )
        return 1

    env = {"DK_PERF_MODULES": ",".join(f"{f}Limits" for f in opts.families)}
    runs = []
    for r in range(opts.runs):
        out = work / f"run-{r + 1}.tsv"
        step = steps.run_logged(
            f"run {r + 1}",
            [str(side.binary), "test", "perfRegression"],
            out,
            cwd=side.src,
            env=env,
            timeout=steps.CTEST_TIMEOUT,
        )
        if not step.ok:
            sys.stdout.write(
                f"tune {name}: FAIL ({step.name} {step.describe_failure()}, see {out})\n"
            )
            return 1
        try:
            runs.append(perfdiff.parse_tsv(out))
        except perfdiff.PerfDiffInputError as exc:
            raise ToolError(str(exc)) from exc

    title = (
        f"tune {name}: {config.label}, {_runs(opts.runs)}, medians of ns/op, "
        f"tree HEAD + working tree ({tree[:10]})"
    )
    return report(work, title, runs, opts)

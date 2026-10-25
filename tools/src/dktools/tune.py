"""Measure each tiered family's speed and memory against its size limit on
this machine, and recommend a limit per family.

Usage::

    dktools tune NAME [--families F[,F...]] [--runs N] [--compiler C]
        [--root DIR] [-j N]

HEAD plus every changed tracked path (as ``verify --all-changed``) is
built under ``ROOT/tune/NAME`` with the compiler's perf preset (gcc15:
its release preset), ``datakit-test`` only.  ``datakit-test test
perfRegression`` then runs ``--runs`` times with ``DK_PERF_MODULES``
naming the families' size-limit sweeps (``<family>Limits``: every
operation at every limit from 256 bytes to 8 KB, rows named
``<op><limit>``).

Per family the result shows, per limit: the median ns/op of every
operation, the filled container's total bytes, and a cost.  The cost is
the geometric mean over the operations of (ns at this limit / the best
ns at any limit), so every operation counts equally and 1.00 means best
at everything.  Limits costing less than ``TIE`` (15%) over the lowest
cost tie on speed; the recommended one holds the data in the fewest
bytes, and the other ties are listed beside it.  The machine's cache sizes are
printed too: a unit that fits a cache level stays hot, which is what the
sweep measures.  The table is also written to ``result.txt`` and the
recommendations to ``result.json``.
"""

from __future__ import annotations

import json
import math
import os
import re
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, gitutil, perfcompare, perfdiff, procs, steps, toolchain
from .gitutil import ToolError

FAMILIES = ("multimap", "multiOrderedSet", "multilist")
#: Limits costing less than this fraction over the best cost tie on speed,
#: and among ties the densest wins: memory first unless a limit is 15% or
#: more slower (a runtime default lets a caller trade back).
TIE = 0.15
#: The dkTierFamily whose process default each family's plain constructor
#: reads (dkTierLimit.h)
DEFAULT_SETTERS = {
    "multimap": "DK_TIER_MULTIMAP",
    "multiOrderedSet": "DK_TIER_MULTIORDEREDSET",
    "multilist": "DK_TIER_MULTILIST",
}

_ROW = re.compile(r"^(?P<op>[A-Za-z]+?)(?P<limit>\d+)$")


@dataclass
class TuneOptions:
    name: str
    families: list[str] = field(default_factory=lambda: list(FAMILIES))
    runs: int = 5
    compiler: toolchain.Compiler = field(
        default_factory=lambda: toolchain.resolve_compiler(toolchain.DEFAULT_COMPILER)
    )
    root: Path | None = None
    jobs: int | None = None


@dataclass
class Sweep:
    """One family's medians: ns[limit][op] and bytes[limit]."""

    ns: dict[int, dict[str, float]] = field(default_factory=dict)
    bytes: dict[int, int] = field(default_factory=dict)

    def ops(self) -> list[str]:
        seen: list[str] = []
        for per in self.ns.values():
            for op in per:
                if op not in seen:
                    seen.append(op)
        return seen

    def costs(self) -> dict[int, float]:
        """Per limit, the geometric mean of ns / best ns over the
        operations every limit measured."""
        ops = [op for op in self.ops() if all(op in per for per in self.ns.values())]
        best = {op: min(per[op] for per in self.ns.values()) for op in ops}
        out = {}
        for limit, per in self.ns.items():
            ratios = [per[op] / best[op] for op in ops if best[op] > 0]
            out[limit] = (
                math.exp(sum(math.log(r) for r in ratios) / len(ratios))
                if ratios
                else math.nan
            )
        return out

    def recommend(self) -> tuple[int, list[int]]:
        """The recommended limit and the other limits that tie with it.

        Limits within TIE of the lowest cost tie on speed; among those the
        one holding the data in the fewest bytes wins (density first), then
        the lower cost."""
        costs = self.costs()
        if not costs:
            raise ToolError("no limit rows to recommend from")
        lowest = min(costs.values())
        tied = [limit for limit in sorted(costs) if costs[limit] < lowest * (1 + TIE)]
        best = min(
            tied,
            key=lambda limit: (self.bytes.get(limit, math.inf), costs[limit]),
        )
        return best, [limit for limit in tied if limit != best]


def sweeps(medians: dict[perfcompare.Key, float]) -> dict[str, Sweep]:
    """The families' sweeps out of perfRegression medians (rows of
    ``<family>Limits`` modules named ``<op><limit>``)."""
    out: dict[str, Sweep] = {}
    for (module, name, nbytes), ns in medians.items():
        if not module.endswith("Limits"):
            continue
        m = _ROW.match(name)
        if not m:
            continue
        sweep = out.setdefault(module[: -len("Limits")], Sweep())
        limit = int(m.group("limit"))
        sweep.ns.setdefault(limit, {})[m.group("op")] = ns
        if nbytes:
            sweep.bytes[limit] = nbytes
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


def render(found: dict[str, Sweep], caches: list[tuple[str, int]]) -> list[str]:
    lines = []
    if caches:
        lines.append(
            "caches    " + ", ".join(f"{label} {_human(n)}" for label, n in caches)
        )
    for family, sweep in found.items():
        ops = sweep.ops()
        costs = sweep.costs()
        best, ties = sweep.recommend()
        lines.append("")
        lines.append(f"{family}: ns/op by size limit (cost 1.00 = best at every op)")
        lines.append(
            f"  {'limit':>6} "
            + " ".join(f"{op:>10}" for op in ops)
            + f" {'bytes':>11} {'cost':>6}"
        )
        for limit in sorted(sweep.ns):
            per = sweep.ns[limit]
            cells = " ".join(
                f"{per[op]:>10.1f}" if op in per else f"{'-':>10}" for op in ops
            )
            mark = "  <- recommended" if limit == best else ""
            lines.append(
                f"  {limit:>6} {cells} {sweep.bytes.get(limit, 0):>11} "
                f"{costs[limit]:>6.2f}{mark}"
            )
        tie_text = f" (within {TIE:.0%}: {', '.join(map(str, ties))})" if ties else ""
        lines.append(f"  recommended limit {best}{tie_text}")
        lines.append(f"  apply: {apply_text(family, best)}")
    return lines


def recommendations(found: dict[str, Sweep]) -> dict[str, dict]:
    out = {}
    for family, sweep in found.items():
        best, ties = sweep.recommend()
        out[family] = {
            "limit": best,
            "ties": ties,
            "apply": apply_text(family, best),
            "cost": {str(k): round(v, 4) for k, v in sweep.costs().items()},
        }
    return out


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


def run(repo: Path, opts: TuneOptions) -> int:
    """Build, sweep, report; the exit code (ToolError: input)."""
    name = gitutil.check_name(opts.name)
    if opts.runs < 1:
        raise ToolError(f"--runs must be at least 1 (got {opts.runs})")
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    config = toolchain.build_config(
        opts.compiler, perfcompare.perf_preset(opts.compiler)
    )
    jobs = opts.jobs or os.cpu_count() or 4

    head = gitutil.git_text(repo, "rev-parse", "HEAD")
    with tempfile.TemporaryDirectory(prefix="dktools-tune-") as tmp:
        tree = gitutil.build_tree(
            repo, Path(tmp) / "index", [(p, None) for p in gitutil.changed_paths(repo)]
        )
    config.compiler.check()

    root = gitutil.scratch_root(opts.root, repo).resolve()
    work = buildstore.fresh_build(repo, root, "tune", name, head, "tune")
    side = perfcompare.Side("tune", tree, work / "src", config)
    gitutil.export_tree(repo, tree, side.src)
    config.check_presets_file(side.src)
    step = perfcompare.build_side(side, work, jobs)
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

    found = sweeps(perfcompare.medians(runs))
    missing = [f for f in opts.families if f not in found]
    if missing:
        raise ToolError(f"no sweep rows for {', '.join(missing)}")
    header = [
        f"tune {name}: {config.label}, {opts.runs} runs, medians of ns/op, "
        f"tree HEAD + working tree ({tree[:10]})"
    ]
    text = "\n".join(header + render(found, cache_sizes())) + "\n"
    (work / "result.txt").write_text(text)
    (work / "result.json").write_text(
        json.dumps(recommendations(found), indent=2) + "\n"
    )
    sys.stdout.write(text)
    return 0

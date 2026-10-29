"""Time two builds of the same perfRegression benchmarks against each other.

Usage::

    dktools perfcompare NAME --base REV [--current REV] [--modules M[,M...]]
        [--runs N] [--tolerance PCT] [--spread] [--bench-path PATH ...]
        [--rename OLD=NEW ...] [--reuse]
        [--compiler C] [--root DIR] [-j N]

Two trees are built under ``ROOT/perfcompare/NAME``:

- ``current``: HEAD plus every changed tracked path and every new file
  under src/ (as ``verify --all-changed``; plain HEAD when nothing
  changed), or with
  ``--current REV`` that revision plus the benchmark sources, so two
  commits are timed apart from whatever else the working tree holds;
- ``base``: REV with the benchmark sources (``--bench-path``, default
  ``src/perfRegression.c`` and ``src/perfRegression.h``) taken from the
  working tree, so both builds time the same benchmark code.

Each is configured with the compiler's perf preset (gcc15: its release
preset) and builds only ``datakit-test``.  Then ``datakit-test test perfRegression`` runs on the
two builds alternately, ``--runs`` times each (the side that runs first
alternates by round), with ``DK_PERF_MODULES`` set to ``--modules`` when
given.  Every run's output is kept as ``run-<side>-<round>.tsv``.

For each row key (module, name, bytes) present on both sides the median
ns_per_op of each side is printed with the change in percent; the table
is also written to ``result.txt``.  A row whose bytes vary between one
side's runs (a heap metered at the allocator) takes that side's median
bytes, and a row whose bytes differ between the sides (a metered heap the
change grew or shrank) is still compared, by module and name, with its
bytes change at the end of the line.  Rows on
one side only are listed.  Each row's verdict compares the two sides'
runs by a rank-sum (Mann-Whitney U) test: ``slower`` or ``faster`` when
the two-sided p is below 0.01, ``~`` when the runs do not tell the sides
apart (a stray run moves a rank, not the verdict), ``-`` with fewer than
five runs a side.
Exits 0, or 1 when ``--tolerance PCT`` is given and some row's current
median exceeds its base median by more than PCT percent with a
``slower`` verdict (or no verdict), or when a build or run fails; 2 on an
input error.
"""

from __future__ import annotations

import dataclasses
import itertools
import math
import re
import statistics
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, gitutil, perfdiff, procs, steps, toolchain
from .gitutil import ToolError

DEFAULT_BENCH_PATHS = ("src/perfRegression.c", "src/perfRegression.h")
_MODULE = re.compile(r"^[A-Za-z0-9_]+$")

Key = tuple[str, str, int]


@dataclass
class PerfCompareOptions:
    name: str
    base: str
    modules: str | None = None
    runs: int = 5
    tolerance: float | None = None
    bench_paths: list[str] = field(default_factory=lambda: list(DEFAULT_BENCH_PATHS))
    compiler: toolchain.Compiler = field(
        default_factory=lambda: toolchain.resolve_compiler(toolchain.DEFAULT_COMPILER)
    )
    root: Path | None = None
    jobs: int | None = None
    #: cmake cache entries (NAME=VALUE) for the current side only, to time
    #: a build option against the same base
    current_defines: tuple[str, ...] = ()
    #: print each row's lowest-highest run per side beside the medians
    spread: bool = False
    #: a revision for the current side (plus the benchmark sources) in
    #: place of HEAD and the working tree
    current: str | None = None
    #: OLD=NEW: the base side's rows of module OLD compared with the
    #: current side's rows of NEW (a benchmark module that was renamed)
    renames: tuple[str, ...] = ()
    #: report again on the runs an earlier perfcompare NAME measured
    reuse: bool = False


def perf_preset(compiler: toolchain.Compiler) -> str | None:
    """``perf`` where the compiler builds it, else its default preset
    (gcc15 builds only ``release``, already optimised)."""
    return "perf" if "perf" in toolchain.COMPILER_PRESETS[compiler.name] else None


def parse_modules(text: str | None) -> str | None:
    """The DK_PERF_MODULES value for `text` (None: every module).

    ToolError for an empty entry or a name that is not a module name."""
    if text is None:
        return None
    names = text.split(",")
    for name in names:
        if not _MODULE.match(name):
            raise ToolError(f"--modules: {name!r} is not a module name")
    return ",".join(names)


def settle_bytes(
    runs: list[dict[Key, perfdiff.PerfRow]],
) -> list[dict[Key, perfdiff.PerfRow]]:
    """One side's runs with each row's bytes made steady: a (module, name)
    every run reports once, with bytes that differ between runs (a heap
    metered at the allocator varies a little), is keyed by the median of
    its bytes in every run, so the runs agree on one row.  Rows a run
    reports more than once, or with the same bytes everywhere, keep their
    keys."""
    seen: dict[tuple[str, str], list[int]] = {}
    repeated: set[tuple[str, str]] = set()
    for rows in runs:
        names: set[tuple[str, str]] = set()
        for key in rows:
            name = key[:2]
            if name in names:
                repeated.add(name)
            names.add(name)
            seen.setdefault(name, []).append(key[2])

    steady: dict[tuple[str, str], int] = {}
    for name, sizes in seen.items():
        if name not in repeated and len(sizes) == len(runs) and len(set(sizes)) > 1:
            steady[name] = int(statistics.median(sizes))

    if not steady:
        return runs
    return [
        {(k[0], k[1], steady.get(k[:2], k[2])): row for k, row in rows.items()}
        for rows in runs
    ]


def medians(runs: list[dict[Key, perfdiff.PerfRow]]) -> dict[Key, float]:
    """Per row key, the median ns_per_op over the runs that have it."""
    values: dict[Key, list[float]] = {}
    for rows in runs:
        for key, row in rows.items():
            values.setdefault(key, []).append(row.ns_per_op)
    return {key: statistics.median(vals) for key, vals in values.items()}


def ranges(runs: list[dict[Key, perfdiff.PerfRow]]) -> dict[Key, tuple[float, float]]:
    """Per row key, the lowest and highest ns_per_op over the runs that
    have it: how far one run can stray from the median"""
    values: dict[Key, list[float]] = {}
    for rows in runs:
        for key, row in rows.items():
            values.setdefault(key, []).append(row.ns_per_op)
    return {key: (min(vals), max(vals)) for key, vals in values.items()}


#: A row's verdict names a direction when its two-sided p is below this
SIGNIFICANCE = 0.01
#: Fewer runs a side than this and a row gets no verdict
MIN_SAMPLES = 5


def samples(runs: list[dict[Key, perfdiff.PerfRow]]) -> dict[Key, list[float]]:
    """Per row key, the ns_per_op of every run that has it"""
    values: dict[Key, list[float]] = {}
    for rows in runs:
        for key, row in rows.items():
            values.setdefault(key, []).append(row.ns_per_op)
    return values


#: Up to this many runs in all, untied samples take the exact U
#: distribution (the normal approximation is too coarse for few runs: at
#: five a side it cannot fall below 0.012)
EXACT_LIMIT = 40


def _u_counts(m: int, n: int) -> list[int]:
    """How many of the C(m+n, m) orderings of m and n untied values give
    each U from 0 to m*n (the recurrence on the largest value's side)"""
    # counts[j] holds the distribution for sizes (i, j) as i grows
    counts = [[1] for _ in range(n + 1)]
    for i in range(1, m + 1):
        row = [[1]]
        for j in range(1, n + 1):
            # The largest value from the first side adds j to U
            with_first = [0] * j + counts[j]
            without = row[j - 1]
            size = i * j + 1
            row.append(
                [
                    (with_first[u] if u < len(with_first) else 0)
                    + (without[u] if u < len(without) else 0)
                    for u in range(size)
                ]
            )
        counts = row
    return counts[n]


#: Up to this many ways to split the runs between the sides, the p is
#: counted over every split (ties included); 8 runs a side is 12870
ENUMERATION_LIMIT = 20000


def _enumerated_p(a: list[float], b: list[float]) -> float:
    """The exact permutation p of the rank sum: the share of the ways to
    choose len(a) of the pooled runs whose midrank sum lies at least as
    far from its mean as 'a''s does (ties take their average rank)"""
    pooled = sorted(a + b)
    midrank: dict[float, float] = {}
    i = 0
    while i < len(pooled):
        j = i
        while j + 1 < len(pooled) and pooled[j + 1] == pooled[i]:
            j += 1
        midrank[pooled[i]] = (i + j) / 2 + 1
        i = j + 1
    ranks = [midrank[v] for v in a + b]
    n1 = len(a)
    mean = n1 * (len(ranks) + 1) / 2
    deviation = abs(sum(ranks[:n1]) - mean) - 1e-9
    total = extreme = 0
    for chosen in itertools.combinations(ranks, n1):
        total += 1
        extreme += abs(sum(chosen) - mean) >= deviation
    return extreme / total


def rank_sum_p(a: list[float], b: list[float]) -> float | None:
    """The two-sided p of the Mann-Whitney U (rank-sum) test that 'a' and
    'b' come from one distribution: counted over every split of the runs
    when there are at most ENUMERATION_LIMIT, else exact from the U
    distribution for untied samples of up to EXACT_LIMIT runs, else
    rank_sum_normal_p.  Ranks, not values, so one stray run moves it by
    one rank.  None when a side has fewer than MIN_SAMPLES."""
    n1, n2 = len(a), len(b)
    if n1 < MIN_SAMPLES or n2 < MIN_SAMPLES:
        return None
    if math.comb(n1 + n2, n1) <= ENUMERATION_LIMIT:
        return _enumerated_p(a, b)
    if n1 + n2 > EXACT_LIMIT or len(set(a) | set(b)) != n1 + n2:
        return rank_sum_normal_p(a, b)
    u = sum(1 for x in a for y in b if x > y)
    counts = _u_counts(n1, n2)
    total = sum(counts)
    below = sum(counts[: u + 1])
    above = sum(counts[u:])
    return min(1.0, 2 * min(below, above) / total)


def rank_sum_normal_p(a: list[float], b: list[float]) -> float:
    """The two-sided Mann-Whitney p by the normal approximation with the
    tie and continuity corrections (samples of MIN_SAMPLES or more)"""
    n1, n2 = len(a), len(b)
    pooled = sorted([(v, 0) for v in a] + [(v, 1) for v in b])
    n = n1 + n2
    rank_a = 0.0
    ties = 0.0
    i = 0
    while i < n:
        j = i
        while j + 1 < n and pooled[j + 1][0] == pooled[i][0]:
            j += 1
        rank = (i + j) / 2 + 1  # the tied values' average rank
        t = j - i + 1
        ties += t**3 - t
        rank_a += rank * sum(1 for k in range(i, j + 1) if pooled[k][1] == 0)
        i = j + 1
    u = rank_a - n1 * (n1 + 1) / 2
    mean = n1 * n2 / 2
    variance = n1 * n2 / 12 * ((n + 1) - ties / (n * (n - 1)))
    if variance <= 0:
        return 1.0
    z = max(abs(u - mean) - 0.5, 0.0) / math.sqrt(variance)
    return math.erfc(z / math.sqrt(2))


def verdict(base: list[float], current: list[float]) -> str:
    """'slower' or 'faster' when the runs differ beyond SIGNIFICANCE,
    '~' when they do not, '-' with too few runs to tell"""
    p = rank_sum_p(base, current)
    if p is None:
        return "-"
    if p >= SIGNIFICANCE:
        return "~"
    return (
        "slower" if statistics.median(current) > statistics.median(base) else "faster"
    )


def change_pct(base: float, current: float) -> float | None:
    """current against base in percent; None when base is 0"""
    if base == 0:
        return None
    return (current - base) / base * 100.0


def bytes_changed(base: dict[Key, float], current: dict[Key, float]) -> dict[Key, Key]:
    """Base keys whose row reports other bytes on the current side (a
    change to a metered heap), each to its current key: a (module, name)
    that is on each side once, with bytes that differ"""

    def one_sided(
        side: dict[Key, float], other: dict[Key, float]
    ) -> dict[tuple[str, str], Key]:
        found: dict[tuple[str, str], list[Key]] = {}
        for key in side:
            if key not in other:
                found.setdefault(key[:2], []).append(key)
        return {name: keys[0] for name, keys in found.items() if len(keys) == 1}

    current_by_name = one_sided(current, base)
    return {
        key: current_by_name[name]
        for name, key in one_sided(base, current).items()
        if name in current_by_name
    }


def table(
    base: dict[Key, float],
    current: dict[Key, float],
    tolerance: float | None,
    spread: tuple[dict[Key, tuple[float, float]], dict[Key, tuple[float, float]]]
    | None = None,
    runs: tuple[dict[Key, list[float]], dict[Key, list[float]]] | None = None,
) -> tuple[list[str], list[Key]]:
    """The comparison lines and the keys that regressed past `tolerance`;
    with `spread` (each side's ranges), each row ends with both sides'
    lowest-highest runs; with `runs` (each side's samples), each row has
    a verdict (verdict()), and a regression past `tolerance` must also be
    a 'slower' verdict.

    Rows keep the base side's key order (perfRegression's order, as
    runs were parsed in it)."""
    lines = [
        f"{'module':<16} {'name':<14} {'bytes':>8} {'base':>10} "
        f"{'current':>10} {'change':>8}"
        + (f" {'verdict':>7}" if runs else "")
        + ("  base range        current range" if spread else "")
    ]
    regressed: list[Key] = []
    partner = bytes_changed(base, current)
    for key, b in base.items():
        ckey = key if key in current else partner.get(key)
        if ckey is None:
            continue
        c = current[ckey]
        pct = change_pct(b, c)
        shown = "-" if pct is None else f"{pct:+.1f}%"
        module, name, nbytes = key
        line = f"{module:<16} {name:<14} {nbytes:>8} {b:>10.1f} {c:>10.1f} {shown:>8}"
        said = verdict(runs[0][key], runs[1][ckey]) if runs else None
        if said is not None:
            line += f" {said:>7}"
        if spread:
            (blo, bhi), (clo, chi) = spread[0][key], spread[1][ckey]
            line += f"  [{blo:.1f}-{bhi:.1f}]".ljust(20) + f"[{clo:.1f}-{chi:.1f}]"
        if ckey != key:
            bpct = change_pct(float(nbytes), float(ckey[2]))
            bshown = "" if bpct is None else f" ({bpct:+.1f}%)"
            line += f"  bytes -> {ckey[2]}{bshown}"
        lines.append(line)
        if (
            tolerance is not None
            and pct is not None
            and pct > tolerance
            and said in (None, "slower", "-")
        ):
            regressed.append(key)
    paired = set(partner.values())
    only_base = [k for k in base if k not in current and k not in partner]
    only_current = [k for k in current if k not in base and k not in paired]
    for label, keys in (("base only", only_base), ("current only", only_current)):
        for module, name, nbytes in keys:
            lines.append(f"{label}: {module} {name} {nbytes}")
    return lines, regressed


@dataclass
class Side:
    """One perf build: its label, tree, export directory and config
    (perfcompare builds two, tune one)."""

    label: str
    tree: str
    src: Path
    config: toolchain.BuildConfig

    @property
    def binary(self) -> Path:
        return self.config.binary_dir(self.src) / "src" / "datakit-test"


def working_side(
    repo: Path, root: Path, kind: str, name: str, config: toolchain.BuildConfig
) -> tuple[Side, Path]:
    """HEAD plus every changed tracked path and every new file under src/,
    exported as one perf build under ROOT/<kind>/<name> (tune and matrix
    measure the working tree this way); the side and its work directory"""
    head = gitutil.git_text(repo, "rev-parse", "HEAD")
    with tempfile.TemporaryDirectory(prefix=f"dktools-{kind}-") as tmp:
        tree = gitutil.build_tree(
            repo, Path(tmp) / "index", [(p, None) for p in gitutil.working_paths(repo)]
        )
    config.compiler.check()
    work = buildstore.fresh_build(repo, root, kind, name, head, kind)
    side = Side(kind, tree, work / "src", config)
    gitutil.export_tree(repo, tree, side.src)
    config.check_presets_file(side.src)
    return side, work


def build_side(side: Side, work: Path, size: procs.Sizing) -> steps.Step:
    """Configure and build `side`'s datakit-test; the failing step or the
    build step."""
    configure = steps.run_logged(
        f"{side.label} configure",
        side.config.configure_command(side.src),
        work / f"{side.label}-configure.log",
        cwd=side.src,
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    if not configure.ok:
        return configure
    return steps.run_logged(
        f"{side.label} build",
        side.config.build_command(side.src, size.build, ["datakit-test"], size.load),
        work / f"{side.label}-build.log",
        cwd=side.src,
        timeout=steps.BUILD_TIMEOUT,
    )


def current_label(
    rev: str | None,
    commit: str | None,
    tree: str,
    bench: list[tuple[str, None]],
) -> str:
    """What a build was made from, for a result header: HEAD and the
    working tree (`commit` None), or `rev` plus the benchmark sources"""
    if commit is None:
        return f"HEAD + working tree ({tree[:10]})"
    return f"{rev} ({commit[:10]}) + {', '.join(p for p, _ in bench)}"


def run(repo: Path, opts: PerfCompareOptions) -> int:
    """Run the comparison; return the exit code (ToolError: input)."""
    name = gitutil.check_name(opts.name)
    if opts.runs < 1:
        raise ToolError(f"--runs must be at least 1 (got {opts.runs})")
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    if opts.tolerance is not None and not opts.tolerance >= 0:
        raise ToolError("--tolerance must be a non-negative number")
    modules = parse_modules(opts.modules)
    parse_renames(list(opts.renames))
    if opts.reuse:
        work = gitutil.scratch_root(opts.root, repo).resolve() / "perfcompare" / name
        runs = load_runs(work)
        sides = work / "sides.txt"
        sides_text = (
            sides.read_text().splitlines()
            if sides.is_file()
            else [f"  reused    {work}"]
        )
        return report(name, work, runs, sides_text, opts)

    bench = [(gitutil.repo_path(p), None) for p in opts.bench_paths]
    config = toolchain.build_config(opts.compiler, perf_preset(opts.compiler))
    current_config = toolchain.build_config(
        opts.compiler, perf_preset(opts.compiler), opts.current_defines
    )
    size = procs.sizing(opts.jobs)

    base_commit = gitutil.resolve_commit(repo, opts.base)
    current_commit = (
        gitutil.resolve_commit(repo, opts.current) if opts.current is not None else None
    )
    with tempfile.TemporaryDirectory(prefix="dktools-perfcompare-") as tmp:
        index = Path(tmp) / "index"
        if current_commit is None:
            current_tree = gitutil.build_tree(
                repo, index, [(p, None) for p in gitutil.working_paths(repo)]
            )
        else:
            current_tree = gitutil.build_tree(repo, index, bench, base=current_commit)
        base_tree = gitutil.build_tree(repo, index, bench, base=base_commit)
    config.compiler.check()

    root = gitutil.scratch_root(opts.root, repo).resolve()
    work = buildstore.fresh_build(
        repo, root, "perfcompare", name, base_commit, "perfcompare"
    )
    sides = []
    for label, tree, side_config in (
        ("base", base_tree, config),
        ("current", current_tree, current_config),
    ):
        src = work / label
        gitutil.export_tree(repo, tree, src)
        side_config.check_presets_file(src)
        sides.append(Side(label, tree, src, side_config))

    for side in sides:
        step = build_side(side, work, size)
        if not step.ok:
            sys.stdout.write(
                f"perfcompare {name}: FAIL ({step.name} {step.describe_failure()}, "
                f"see {step.log})\n"
            )
            return 1

    env = {"DK_PERF_MODULES": modules} if modules else {}
    runs: dict[str, list[dict[Key, perfdiff.PerfRow]]] = {"base": [], "current": []}
    for r in range(opts.runs):
        order = sides if r % 2 == 0 else list(reversed(sides))
        for side in order:
            out = work / f"run-{side.label}-{r + 1}.tsv"
            step = steps.run_logged(
                f"{side.label} run {r + 1}",
                [str(side.binary), "test", "perfRegression"],
                out,
                cwd=side.src,
                env=env,
                timeout=steps.CTEST_TIMEOUT,
            )
            if not step.ok:
                sys.stdout.write(
                    f"perfcompare {name}: FAIL ({step.name} "
                    f"{step.describe_failure()}, see {out})\n"
                )
                return 1
            try:
                runs[side.label].append(perfdiff.parse_tsv(out))
            except perfdiff.PerfDiffInputError as exc:
                raise ToolError(str(exc)) from exc

    sides_text = [
        f"  base      {opts.base} ({base_commit[:10]}) + {', '.join(p for p, _ in bench)}",
        f"  current   {current_label(opts.current, current_commit, current_tree, bench)}",
        f"  config    {config.label}, {opts.runs} runs each, medians of ns/op"
        + (f", modules {modules}" if modules else ""),
    ]
    (work / "sides.txt").write_text("\n".join(sides_text) + "\n")
    return report(name, work, runs, sides_text, opts)


def parse_renames(texts: list[str] | None) -> dict[str, str]:
    """--rename OLD=NEW values as {OLD: NEW}; ToolError for a malformed
    one or a name that is not a module name"""
    renames: dict[str, str] = {}
    for text in texts or []:
        old, sep, new = text.partition("=")
        if not sep or not _MODULE.match(old) or not _MODULE.match(new):
            raise ToolError(f"--rename takes OLD=NEW module names (got {text!r})")
        renames[old] = new
    return renames


def rename_modules(
    runs: list[dict[Key, perfdiff.PerfRow]], renames: dict[str, str]
) -> list[dict[Key, perfdiff.PerfRow]]:
    """'runs' with each row of a module named in 'renames' under its new
    name, so a base row meets the current row that replaced it"""
    if not renames:
        return runs
    renamed = []
    for one in runs:
        out: dict[Key, perfdiff.PerfRow] = {}
        for (module, row_name, nbytes), row in one.items():
            module = renames.get(module, module)
            out[(module, row_name, nbytes)] = dataclasses.replace(row, module=module)
        renamed.append(out)
    return renamed


_RUN_FILE = re.compile(r"^run-(base|current)-(\d+)\.tsv$")


def load_runs(work: Path) -> dict[str, list[dict[Key, perfdiff.PerfRow]]]:
    """The run-<side>-N.tsv files an earlier perfcompare left in 'work',
    per side in run order"""
    found: dict[str, list[tuple[int, Path]]] = {"base": [], "current": []}
    for path in work.glob("run-*.tsv"):
        if m := _RUN_FILE.match(path.name):
            found[m.group(1)].append((int(m.group(2)), path))
    if not found["base"] or not found["current"]:
        raise ToolError(f"--reuse: {work} holds no run-base-N/run-current-N.tsv")
    try:
        return {
            side: [perfdiff.parse_tsv(p) for _, p in sorted(files)]
            for side, files in found.items()
        }
    except perfdiff.PerfDiffInputError as exc:
        raise ToolError(str(exc)) from exc


def report(
    name: str,
    work: Path,
    runs: dict[str, list[dict[Key, perfdiff.PerfRow]]],
    sides_text: list[str],
    opts: PerfCompareOptions,
) -> int:
    """Compare the sides' runs, print the table and write result.txt; the
    exit code"""
    renames = parse_renames(list(opts.renames))
    base_runs = settle_bytes(rename_modules(runs["base"], renames))
    current_runs = settle_bytes(runs["current"])
    lines, regressed = table(
        medians(base_runs),
        medians(current_runs),
        opts.tolerance,
        (ranges(base_runs), ranges(current_runs)) if opts.spread else None,
        (samples(base_runs), samples(current_runs)),
    )
    verdict = "REGRESSION" if regressed else "ok"
    header = [f"perfcompare {name}: {verdict}", *sides_text]
    if renames:
        header.append(
            "  renamed   "
            + ", ".join(f"base {old} as {new}" for old, new in renames.items())
        )
    text = "\n".join(header + lines) + "\n"
    (work / "result.txt").write_text(text)
    sys.stdout.write(text)
    return 1 if regressed else 0

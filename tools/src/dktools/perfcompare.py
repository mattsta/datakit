"""Time two builds of the same perfRegression benchmarks against each other.

Usage::

    dktools perfcompare NAME --base REV [--modules M[,M...]] [--runs N]
        [--tolerance PCT] [--spread] [--bench-path PATH ...] [--compiler C]
        [--root DIR] [-j N]

Two trees are built under ``ROOT/perfcompare/NAME``:

- ``current``: HEAD plus every changed tracked path (as ``verify
  --all-changed``; plain HEAD when nothing changed);
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
is also written to ``result.txt``.  Rows on one side only are listed.
Exits 0, or 1 when ``--tolerance PCT`` is given and some row's current
median exceeds its base median by more than PCT percent (the rows
perfdiff would call regressed), or when a build or run fails; 2 on an
input error.
"""

from __future__ import annotations

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


def change_pct(base: float, current: float) -> float | None:
    """current against base in percent; None when base is 0"""
    if base == 0:
        return None
    return (current - base) / base * 100.0


def table(
    base: dict[Key, float],
    current: dict[Key, float],
    tolerance: float | None,
    spread: tuple[dict[Key, tuple[float, float]], dict[Key, tuple[float, float]]]
    | None = None,
) -> tuple[list[str], list[Key]]:
    """The comparison lines and the keys that regressed past `tolerance`;
    with `spread` (each side's ranges), each row ends with both sides'
    lowest-highest runs.

    Rows keep the base side's key order (perfRegression's order, as
    runs were parsed in it)."""
    lines = [
        f"{'module':<16} {'name':<14} {'bytes':>8} {'base':>10} "
        f"{'current':>10} {'change':>8}"
        + ("  base range        current range" if spread else "")
    ]
    regressed: list[Key] = []
    for key, b in base.items():
        if key not in current:
            continue
        c = current[key]
        pct = change_pct(b, c)
        shown = "-" if pct is None else f"{pct:+.1f}%"
        module, name, nbytes = key
        line = f"{module:<16} {name:<14} {nbytes:>8} {b:>10.1f} {c:>10.1f} {shown:>8}"
        if spread:
            (blo, bhi), (clo, chi) = spread[0][key], spread[1][key]
            line += f"  [{blo:.1f}-{bhi:.1f}]".ljust(20) + f"[{clo:.1f}-{chi:.1f}]"
        lines.append(line)
        if tolerance is not None and pct is not None and pct > tolerance:
            regressed.append(key)
    only_base = [k for k in base if k not in current]
    only_current = [k for k in current if k not in base]
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
    bench = [(gitutil.repo_path(p), None) for p in opts.bench_paths]
    config = toolchain.build_config(opts.compiler, perf_preset(opts.compiler))
    current_config = toolchain.build_config(
        opts.compiler, perf_preset(opts.compiler), opts.current_defines
    )
    size = procs.sizing(opts.jobs)

    base_commit = gitutil.resolve_commit(repo, opts.base)
    with tempfile.TemporaryDirectory(prefix="dktools-perfcompare-") as tmp:
        index = Path(tmp) / "index"
        current_tree = gitutil.build_tree(
            repo, index, [(p, None) for p in gitutil.changed_paths(repo)]
        )
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

    lines, regressed = table(
        medians(runs["base"]),
        medians(runs["current"]),
        opts.tolerance,
        (ranges(runs["base"]), ranges(runs["current"])) if opts.spread else None,
    )
    verdict = "REGRESSION" if regressed else "ok"
    header = [
        f"perfcompare {name}: {verdict}",
        f"  base      {opts.base} ({base_commit[:10]}) + {', '.join(p for p, _ in bench)}",
        f"  current   HEAD + working tree ({current_tree[:10]})",
        f"  config    {config.label}, {opts.runs} runs each, medians of ns/op"
        + (f", modules {modules}" if modules else ""),
    ]
    text = "\n".join(header + lines) + "\n"
    (work / "result.txt").write_text(text)
    sys.stdout.write(text)
    return 1 if regressed else 0

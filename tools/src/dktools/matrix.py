"""Measure every container's create, read, update and delete throughput
and bytes per element, per element kind and size, and render the
comparison as markdown (docs/PERFORMANCE_MATRIX.md).

Usage::

    dktools matrix NAME [--runs N] [--write] [--reuse] [--compiler C]
        [--root DIR] [-j N]

HEAD plus every changed tracked path and every new file under src/ is
built under ``ROOT/matrix/NAME`` with the compiler's perf preset (gcc15:
its release preset), ``datakit-test`` only.  ``datakit-test test
containerMatrix`` then runs ``--runs`` times (default 3) with
``DK_MATRIX=full``: each container, element kind and size measured
phase by phase, every result checked by the test (a failed check fails
the run).  Each run's output is kept as ``run-N.txt``.

The test prints one row per measured phase::

    MATRIX <container> <kind> <size> <op> <ops> <seconds> <bytes_per_element>

and one ``MATRIXSHAPE <container> <shape>`` line per container (how its
operations map onto create, read, update and delete).  Per (container,
kind, size, op) the report takes the median seconds over the runs and
gives operations per second; bytes per element are the container's bytes
after create over its element count.  Per kind and size one table ranks
the containers, the fastest per operation and the smallest in bold.

The report is written to ``result.md`` and printed; ``--write`` also
writes it to ``docs/PERFORMANCE_MATRIX.md``.  ``--reuse`` reports again
on the runs an earlier ``matrix NAME`` left, without building.
"""

from __future__ import annotations

import datetime
import platform
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import gitutil, perfcompare, procs, steps, toolchain, tune
from .gitutil import ToolError

#: The phases, in the order the test runs them
OPS = ("create", "read", "update", "delete")
#: Where --write puts the report
DOC = Path("docs/PERFORMANCE_MATRIX.md")

#: (container, kind, size, op)
Key = tuple[str, str, int, str]


@dataclass
class Run:
    """One run's rows: per key, (ops, seconds, bytes per element)"""

    rows: dict[Key, tuple[int, float, float]] = field(default_factory=dict)
    shapes: dict[str, str] = field(default_factory=dict)


def parse(text: str, source: str = "output") -> Run:
    """The MATRIX and MATRIXSHAPE lines of one run (other lines skipped;
    the column header row too); ToolError on a malformed row"""
    run = Run()
    for number, line in enumerate(text.splitlines(), 1):
        fields = line.split("\t")
        if fields[0] == "MATRIXSHAPE":
            if len(fields) != 3:
                raise ToolError(f"{source}:{number}: MATRIXSHAPE needs 2 fields")
            run.shapes[fields[1]] = fields[2]
            continue
        if fields[0] != "MATRIX" or fields[1:2] == ["container"]:
            continue
        try:
            if len(fields) != 8 or fields[4] not in OPS:
                raise ValueError
            key = (fields[1], fields[2], int(fields[3]), fields[4])
            run.rows[key] = (int(fields[5]), float(fields[6]), float(fields[7]))
        except ValueError:
            raise ToolError(
                f"{source}:{number}: malformed MATRIX row: {line!r}"
            ) from None
    return run


@dataclass
class Cell:
    """One phase's medians over the runs"""

    ops_per_sec: float
    bytes_per_element: float


def medians(runs: list[Run]) -> dict[Key, Cell]:
    """Per key present in every run: operations per second at the median
    seconds, and the median bytes per element"""
    if not runs:
        return {}
    keys = [k for k in runs[0].rows if all(k in r.rows for r in runs[1:])]
    cells = {}
    for key in keys:
        ops = runs[0].rows[key][0]
        secs = statistics.median(r.rows[key][1] for r in runs)
        nbytes = statistics.median(r.rows[key][2] for r in runs)
        cells[key] = Cell(ops / secs if secs > 0 else float("inf"), nbytes)
    return cells


def rate(ops_per_sec: float) -> str:
    """'12.3M', '850K', '4.10G': three significant digits"""
    if ops_per_sec == float("inf"):
        return "inf"
    for scale, suffix in ((1e9, "G"), (1e6, "M"), (1e3, "K")):
        if ops_per_sec >= scale:
            value = ops_per_sec / scale
            break
    else:
        value, suffix = ops_per_sec, ""
    digits = 0 if value >= 100 else 1 if value >= 10 else 2
    return f"{value:.{digits}f}{suffix}"


def _size(n: int) -> str:
    return f"{n:,}"


def tables(cells: dict[Key, Cell], shapes: dict[str, str]) -> list[str]:
    """Per kind and size, a markdown table: one row per container, its
    operations per second per phase and bytes per element, the best of
    each column in bold"""
    containers = list(dict.fromkeys(k[0] for k in cells))
    kinds = list(dict.fromkeys(k[1] for k in cells))
    lines: list[str] = []
    for kind in kinds:
        sizes = sorted({k[2] for k in cells if k[1] == kind})
        for size in sizes:
            here = [c for c in containers if (c, kind, size, "create") in cells]
            if not here:
                continue
            best = {
                op: max(
                    (
                        cells[(c, kind, size, op)].ops_per_sec
                        for c in here
                        if (c, kind, size, op) in cells
                    ),
                    default=None,
                )
                for op in OPS
            }
            smallest = min(
                cells[(c, kind, size, "create")].bytes_per_element for c in here
            )
            lines += [
                "",
                f"### {kind}, {_size(size)} elements",
                "",
                "| container | shape | " + " | ".join(OPS) + " | bytes/element |",
                "|---|---|" + "---:|" * (len(OPS) + 1),
            ]
            for c in here:
                row = [c, shapes.get(c, "")]
                for op in OPS:
                    cell = cells.get((c, kind, size, op))
                    if cell is None:
                        row.append("—")
                        continue
                    text = rate(cell.ops_per_sec)
                    row.append(f"**{text}**" if cell.ops_per_sec == best[op] else text)
                nbytes = cells[(c, kind, size, "create")].bytes_per_element
                text = f"{nbytes:.1f}"
                row.append(f"**{text}**" if nbytes == smallest else text)
                lines.append("| " + " | ".join(row) + " |")
    return lines


def cpu_name() -> str:
    """The processor's name as the system reports it, else the machine"""
    probes = (
        (["sysctl", "-n", "machdep.cpu.brand_string"], None),
        (
            ["cat", "/proc/cpuinfo"],
            re.compile(r"^model name\s*:\s*(.+)$", re.MULTILINE),
        ),
    )
    for command, pattern in probes:
        try:
            done = procs.run(command, timeout=10)
        except (procs.ToolError, procs.TimedOut, OSError):
            continue
        text = done.stdout.decode(errors="replace").strip()
        if done.returncode == 0 and text:
            if pattern is None:
                return text
            if m := pattern.search(text):
                return m.group(1).strip()
    return platform.machine() or "unknown"


def render(
    cells: dict[Key, Cell],
    shapes: dict[str, str],
    config: str,
    runs: int,
    machine: str,
    caches: list[tuple[str, int]],
    when: str,
) -> str:
    """The whole report as markdown"""
    cache_text = ", ".join(f"{label} {tune.human_bytes(n)}" for label, n in caches)
    head = [
        "# Container performance matrix",
        "",
        "Generated by `dktools matrix NAME --write` (tools/README.md); do not",
        "edit by hand. For what the numbers mean for choosing a container,",
        "see [CHOOSING_A_CONTAINER.md](CHOOSING_A_CONTAINER.md).",
        "",
        f"- Measured {when} on {machine}"
        + (f" ({cache_text})" if cache_text else "")
        + f", {config}, median of {runs} run{'' if runs == 1 else 's'}.",
        "- Each cell is operations per second for one phase over every",
        "  element: create inserts them (keyed containers in a shuffled order,",
        "  sequences append), read looks each up in a shuffled order and",
        "  checks its value, update replaces each value, delete removes each",
        "  (sequences pop the tail). The test checks every result.",
        "- bytes/element is the container's own bytes after create over its",
        "  element count. An element is one value of the kind (8 bytes for an",
        "  integer, 16 or 256 for a string) in the sequences, sets and the",
        "  pool; a key and a value of the kind in multimap and multidict; a",
        "  member of the kind with an integer score in multiOrderedSet; and",
        "  only recency metadata in multilru, which stores no values.",
        "- int-small is 0, 2, 4, ...; int-wide spans the whole 64-bit range;",
        "  str16 and str256 are byte strings of that length.",
        "- The fastest per column and the smallest bytes/element are in bold.",
        "  — means the container has no update (sets keep only keys).",
    ]
    return "\n".join([*head, *tables(cells, shapes)]) + "\n"


_RUN_FILE = re.compile(r"^run-(\d+)\.txt$")


def load_runs(work: Path) -> list[Run]:
    """The run-N.txt files an earlier matrix left in 'work', in order"""
    files = sorted(
        (int(m.group(1)), p)
        for p in work.glob("run-*.txt")
        if (m := _RUN_FILE.match(p.name))
    )
    if not files:
        raise ToolError(f"--reuse: {work} holds no run-N.txt from an earlier matrix")
    return [parse(p.read_text(errors="replace"), str(p)) for _, p in files]


@dataclass
class MatrixOptions:
    name: str
    runs: int
    write: bool
    reuse: bool
    compiler: toolchain.Compiler
    root: Path | None
    jobs: int | None


def report(repo: Path, work: Path, runs: list[Run], config: str, write: bool) -> int:
    """Render 'runs', print it and write result.md (and the doc)"""
    cells = medians(runs)
    if not cells:
        raise ToolError(f"no MATRIX rows in the runs under {work}")
    shapes = {k: v for r in runs for k, v in r.shapes.items()}
    text = render(
        cells,
        shapes,
        config,
        len(runs),
        cpu_name(),
        tune.cache_sizes(),
        datetime.date.today().isoformat(),
    )
    (work / "result.md").write_text(text)
    sys.stdout.write(text)
    if write:
        (repo / DOC).write_text(text)
        sys.stdout.write(f"matrix: wrote {DOC}\n")
    return 0


def run(repo: Path, opts: MatrixOptions) -> int:
    """Build, measure, report; the exit code (ToolError: input)"""
    name = gitutil.check_name(opts.name)
    if opts.runs < 1:
        raise ToolError(f"--runs must be at least 1 (got {opts.runs})")
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    root = gitutil.scratch_root(opts.root, repo).resolve()
    config = toolchain.build_config(
        opts.compiler, perfcompare.perf_preset(opts.compiler)
    )

    if opts.reuse:
        work = root / "matrix" / name
        meta = work / "config.txt"
        label = meta.read_text().strip() if meta.is_file() else "unknown config"
        return report(repo, work, load_runs(work), label, opts.write)

    side, work = perfcompare.working_side(repo, root, "matrix", name, config)
    (work / "config.txt").write_text(config.label + "\n")
    step = perfcompare.build_side(side, work, procs.sizing(opts.jobs))
    if not step.ok:
        sys.stdout.write(
            f"matrix {name}: FAIL ({step.name} {step.describe_failure()}, see {step.log})\n"
        )
        return 1

    runs = []
    for r in range(opts.runs):
        out = work / f"run-{r + 1}.txt"
        step = steps.run_logged(
            f"run {r + 1}",
            [str(side.binary), "test", "containerMatrix"],
            out,
            cwd=side.src,
            env={"DK_MATRIX": "full"},
            timeout=steps.CTEST_TIMEOUT,
        )
        if not step.ok:
            sys.stdout.write(
                f"matrix {name}: FAIL ({step.name} {step.describe_failure()}, see {out})\n"
            )
            return 1
        runs.append(parse(out.read_text(errors="replace"), str(out)))
    return report(repo, work, runs, config.label, opts.write)

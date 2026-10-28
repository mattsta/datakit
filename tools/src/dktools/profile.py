"""Where perfRegression rows spend their time: a sampled profile.

Usage::

    dktools profile NAME --modules M[,M...] [--rev REV] [--top N]
        [--compiler C] [--root DIR] [-j N]

HEAD plus every changed tracked path (as perfcompare's ``current`` side),
or with ``--rev REV`` that revision plus the benchmark sources (as
perfcompare's base side, so two profiles show where a change moved the
time), is built with the compiler's perf preset under
``ROOT/profile/NAME``.
``datakit-test test perfRegression`` then runs once with
``DK_PERF_MODULES`` set to ``--modules``, watched by the platform's
sampling profiler:

- macOS: ``sample PID`` (part of the base system), attached once the run
  starts and stopped once it ends; its report is read back;
- Linux: ``perf record -g``, read back with ``perf script``.

The recording stays in the work directory.  Printed (and written to
``result.txt``): the ``--top`` functions by self samples (the sampled
frame) and by inclusive samples (anywhere on the stack, once per sample
however deep it recurses), each as a percent of all samples.  Exits 0;
1 when the build, the run or the profiler fails or no sample was read;
2 on an input error.
"""

from __future__ import annotations

import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, gitutil, perfcompare, procs, steps, toolchain
from .gitutil import ToolError

#: how long ``sample`` may take to finish once the run has ended
SAMPLE_GRACE = 60.0

#: a call-graph line of a ``sample`` report: tree marks, a count, a name
_SAMPLE_NODE = re.compile(r"^(?P<lead>[ +!:|]*)(?P<count>\d+) (?P<name>.*?)(  \(in |$)")
#: a "Sort by top of stack" line: a name, its library, a count
_SAMPLE_TOP = re.compile(r"^\s+(?P<name>.*?)  \(in .*?\)\s+(?P<count>\d+)\s*$")


@dataclass
class ProfileOptions:
    name: str
    modules: str
    top: int = 25
    compiler: toolchain.Compiler = field(
        default_factory=lambda: toolchain.resolve_compiler(toolchain.DEFAULT_COMPILER)
    )
    root: Path | None = None
    jobs: int | None = None
    #: a revision (plus the benchmark sources) in place of HEAD and the
    #: working tree
    rev: str | None = None


@dataclass
class Samples:
    """Sample weight per function: as the sampled frame, and anywhere on
    the stack (a function counted once per sample however often it
    recurses)"""

    self_weight: Counter[str] = field(default_factory=Counter)
    inclusive: Counter[str] = field(default_factory=Counter)
    total: int = 0

    def add(self, stack: list[str], weight: int) -> None:
        """One sample: 'stack' leaf first"""
        if not stack:
            return
        self.total += weight
        self.self_weight[stack[0]] += weight
        for name in set(stack):
            self.inclusive[name] += weight


def parse_sample(text: str) -> Samples:
    """The samples of a macOS ``sample`` report.

    Inclusive weights come from its call graph, a tree whose lines are
    indented by tree marks and carry the samples under that node: a node
    counts unless the same function is already above it on its path.
    The total is the threads' root counts.  Self weights come from its
    "Sort by top of stack" list."""
    samples = Samples()
    section = None
    path: list[tuple[int, str]] = []
    for line in text.splitlines():
        if line.startswith("Call graph:"):
            section = "graph"
            continue
        if line.startswith("Sort by top of stack"):
            section = "top"
            continue
        if not line.strip():
            section = None
            continue
        if section == "graph":
            m = _SAMPLE_NODE.match(line)
            if not m:
                continue
            depth = len(m.group("lead"))
            count = int(m.group("count"))
            name = m.group("name").strip()
            while path and path[-1][0] >= depth:
                path.pop()
            if name.startswith("Thread_"):
                samples.total += count
            elif name not in {n for _, n in path}:
                samples.inclusive[name] += count
            path.append((depth, name))
        elif section == "top":
            m = _SAMPLE_TOP.match(line)
            if m:
                samples.self_weight[m.group("name").strip()] += int(m.group("count"))
    return samples


def parse_perf_script(text: str) -> Samples:
    """The samples of ``perf script`` output: a header line per sample,
    then one frame line per stack entry (leaf first: address, symbol,
    module), then a blank line"""
    samples = Samples()
    stack: list[str] = []
    in_sample = False
    for line in [*text.splitlines(), ""]:
        if not line.strip():
            if in_sample:
                samples.add(stack, 1)
            stack = []
            in_sample = False
            continue
        if not line[0].isspace():
            in_sample = True
            continue
        parts = line.split()
        if len(parts) >= 2:
            symbol = parts[1].split("+0x", 1)[0]
            stack.append(symbol)
    return samples


def render(samples: Samples, top: int) -> list[str]:
    """The two top-N tables"""
    lines = [f"{samples.total} sample weight"]
    if samples.total == 0:
        return lines
    for title, counts in (
        ("self", samples.self_weight),
        ("inclusive", samples.inclusive),
    ):
        lines.append(f"{title}:")
        for name, weight in counts.most_common(top):
            lines.append(f"  {weight * 100.0 / samples.total:6.2f}%  {name}")
    return lines


def _fail(name: str, what: str, log: Path) -> int:
    sys.stdout.write(f"profile {name}: FAIL ({what}, see {log})\n")
    return 1


def _record_sample(
    name: str, command: list[str], work: Path, cwd: Path, env: dict[str, str]
) -> tuple[int, Path]:
    """macOS: the run, with ``sample`` attached to it until it ends; the
    exit code and the report"""
    report = work / "sample.txt"
    run_log = work / "profile-run.log"
    sample_log = work / "profile-sample.log"
    with (
        run_log.open("wb") as run_out,
        sample_log.open("wb") as sample_out,
        procs.started(command, run_out, cwd=cwd, env=env) as target,
    ):
        sampler_cmd = [
            "sample",
            str(target.pid),
            str(int(steps.CTEST_TIMEOUT)),
            "1",
            "-mayDie",
            "-file",
            str(report),
        ]
        with procs.started(sampler_cmd, sample_out) as sampler:
            target.wait()
            try:
                sampler.wait(timeout=SAMPLE_GRACE)
            except subprocess.TimeoutExpired:
                # Interrupted, sample stops and writes its report
                os.kill(sampler.pid, signal.SIGINT)
                sampler.wait(timeout=SAMPLE_GRACE)
    if target.returncode != 0:
        return _fail(name, f"run exit {target.returncode}", run_log), report
    if not report.exists():
        return _fail(name, f"sample exit {sampler.returncode}", sample_log), report
    return 0, report


def _record_perf(
    name: str, command: list[str], work: Path, cwd: Path, env: dict[str, str]
) -> tuple[int, Path]:
    """Linux: ``perf record`` around the run, then ``perf script``"""
    data = work / "perf.data"
    report = work / "perf-script.txt"
    record = steps.run_logged(
        "profile record",
        ["perf", "record", "-g", "-o", str(data), "--", *command],
        work / "profile-record.log",
        cwd=cwd,
        env=env,
        timeout=steps.CTEST_TIMEOUT,
    )
    if not record.ok:
        return _fail(
            name, f"{record.name} {record.describe_failure()}", record.log
        ), report
    script = steps.run_logged(
        "profile script",
        ["perf", "script", "-i", str(data)],
        report,
        cwd=cwd,
        timeout=steps.CTEST_TIMEOUT,
    )
    if not script.ok:
        return _fail(name, f"{script.name} {script.describe_failure()}", report), report
    return 0, report


def run(repo: Path, opts: ProfileOptions) -> int:
    """Profile the modules; return the exit code (ToolError: input)."""
    name = gitutil.check_name(opts.name)
    if opts.top < 1:
        raise ToolError(f"--top must be at least 1 (got {opts.top})")
    modules = perfcompare.parse_modules(opts.modules)
    darwin = platform.system() == "Darwin"
    tool = "sample" if darwin else "perf"
    if shutil.which(tool) is None:
        raise ToolError(f"{tool}: not found on PATH")
    config = toolchain.build_config(
        opts.compiler, perfcompare.perf_preset(opts.compiler)
    )
    size = procs.sizing(opts.jobs)

    head = gitutil.resolve_commit(repo, "HEAD")
    commit = gitutil.resolve_commit(repo, opts.rev) if opts.rev is not None else None
    bench = [(gitutil.repo_path(p), None) for p in perfcompare.DEFAULT_BENCH_PATHS]
    with tempfile.TemporaryDirectory(prefix="dktools-profile-") as tmp:
        index = Path(tmp) / "index"
        if commit is None:
            tree = gitutil.build_tree(
                repo, index, [(p, None) for p in gitutil.changed_paths(repo)]
            )
        else:
            tree = gitutil.build_tree(repo, index, bench, base=commit)
    config.compiler.check()

    root = gitutil.scratch_root(opts.root, repo).resolve()
    work = buildstore.fresh_build(
        repo, root, "profile", name, commit or head, "profile"
    )
    src = work / "tree"
    gitutil.export_tree(repo, tree, src)
    config.check_presets_file(src)
    side = perfcompare.Side("profile", tree, src, config)
    step = perfcompare.build_side(side, work, size)
    if not step.ok:
        return _fail(name, f"{step.name} {step.describe_failure()}", step.log)

    command = [str(side.binary), "test", "perfRegression"]
    env = {**os.environ, "DK_PERF_MODULES": modules}
    record = _record_sample if darwin else _record_perf
    code, report = record(name, command, work, src, env)
    if code:
        return code

    text = report.read_text(errors="replace")
    samples = parse_sample(text) if darwin else parse_perf_script(text)
    header = [
        f"profile {name}: {config.label}, modules {modules}",
        f"  tree      {perfcompare.current_label(opts.rev, commit, tree, bench)}",
        f"  recording {report}",
    ]
    lines = header + render(samples, opts.top)
    if not samples.total:
        lines.append(
            "  no samples: the run may have ended before the profiler "
            "attached (rows need about a second or more)"
        )
    out = "\n".join(lines) + "\n"
    (work / "result.txt").write_text(out)
    sys.stdout.write(out)
    return 0 if samples.total else 1

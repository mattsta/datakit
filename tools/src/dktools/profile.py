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

- macOS: ``sample PID`` (part of the base system), which takes a moment
  to start and reads stacks from a live process: the run holds at start
  until it is sampling (``DK_PROFILE_HOLD_FDS``) and at exit until it
  has reported (the sampler is stopped while the process lives), and
  the holds' own frames are left out; the report is read back;
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
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, gitutil, perfcompare, procs, steps, toolchain
from .gitutil import ToolError

#: how long ``sample`` may take to finish once the run has ended
SAMPLE_GRACE = 60.0
#: seconds ``sample`` takes to start sampling, before the run is let go
SAMPLE_START = 1.5
#: frames of the run's own waiting (datakit-test.c's profile hold),
#: left out of the profile with everything under them
SAMPLE_HOLD_FRAMES = ("profileHoldAtExit_", "profileHoldSetup_")

#: a call-graph line of a ``sample`` report: tree marks, a count, a name
_SAMPLE_NODE = re.compile(r"^(?P<lead>[ +!:|]*)(?P<count>\d+) (?P<name>.*?)(  \(in |$)")


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

    def merge(self, other: Samples) -> None:
        """Add another recording's samples (one profile from many reports)"""
        self.total += other.total
        self.self_weight.update(other.self_weight)
        self.inclusive.update(other.inclusive)


def parse_sample(text: str, exclude: tuple[str, ...] = ()) -> Samples:
    """The samples of a macOS ``sample`` report, read from its call graph:
    a tree whose lines are indented by tree marks and carry the samples
    under that node.  A node's self weight is its count less its
    children's; it counts inclusively unless the same function is already
    above it on its path.  The total is the threads' root counts.  A node
    named in 'exclude' leaves the profile with everything under it (its
    count comes off every node above it): a run's own waiting, such as the
    profile hold at exit."""
    samples = Samples()
    in_graph = False
    # open nodes, outermost first: [depth, name, count, children's count]
    path: list[list] = []
    skip_below: int | None = None

    def close(node: list) -> None:
        depth, name, count, children = node
        if name.startswith("Thread_"):
            samples.total += count
            return
        if count - children > 0:
            samples.self_weight[name] += count - children
        if count > 0 and name not in {n[1] for n in path}:
            samples.inclusive[name] += count

    for line in text.splitlines():
        if line.startswith("Call graph:"):
            in_graph = True
            continue
        if not in_graph:
            continue
        if not line.strip():
            break
        m = _SAMPLE_NODE.match(line)
        if not m:
            continue
        depth = len(m.group("lead"))
        if skip_below is not None and depth > skip_below:
            continue
        skip_below = None
        while path and path[-1][0] >= depth:
            close(path.pop())
        count = int(m.group("count"))
        name = m.group("name").strip()
        if name in exclude:
            for i, node in enumerate(path):
                node[2] -= count
                if i < len(path) - 1:
                    node[3] -= count
            skip_below = depth
            continue
        if path:
            path[-1][3] += count
        path.append([depth, name, count, 0])
    while path:
        close(path.pop())
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
) -> tuple[int, list[Path]]:
    """macOS: the run, sampled from when the sampler samples until its work
    is done; the exit code and the report"""
    run_log = work / "profile-run.log"
    sample_log = work / "profile-sample.log"
    # sample writes a report from a live process, and an interrupted one
    # can stall: it runs in fixed windows that end on their own, while the
    # run holds at exit (DK_PROFILE_HOLD_FDS, datakit-test.c) until the
    # window it finished in has reported
    done_r, done_w = os.pipe()
    release_r, release_w = os.pipe()
    env = {**env, "DK_PROFILE_HOLD_FDS": f"{done_w},{release_r}"}
    reports: list[Path] = []
    with (
        run_log.open("wb") as run_out,
        sample_log.open("wb") as sample_out,
        procs.started(
            command, run_out, cwd=cwd, env=env, pass_fds=(done_w, release_r)
        ) as target,
    ):
        os.close(done_w)
        os.close(release_r)
        report = work / "sample.txt"
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
            try:
                # The run starts work once the sampler is sampling (an
                # interrupt before then can stall it); its done byte (or
                # EOF, if it died) stops the sampler, which reports while
                # the run still holds
                time.sleep(SAMPLE_START)
                os.write(release_w, b"g")
                os.read(done_r, 1)
                if sampler.poll() is None:
                    os.kill(sampler.pid, signal.SIGINT)
                try:
                    sampler.wait(timeout=SAMPLE_GRACE)
                except subprocess.TimeoutExpired:
                    os.kill(sampler.pid, signal.SIGKILL)
                    sampler.wait()
            finally:
                os.close(done_r)
                os.close(release_w)
        if report.exists():
            reports.append(report)
    if target.returncode != 0:
        return _fail(name, f"run exit {target.returncode}", run_log), reports
    if not reports:
        return _fail(name, "sample wrote no report", sample_log), reports
    return 0, reports


def _record_perf(
    name: str, command: list[str], work: Path, cwd: Path, env: dict[str, str]
) -> tuple[int, list[Path]]:
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
        return _fail(name, f"{record.name} {record.describe_failure()}", record.log), [
            report
        ]
    script = steps.run_logged(
        "profile script",
        ["perf", "script", "-i", str(data)],
        report,
        cwd=cwd,
        timeout=steps.CTEST_TIMEOUT,
    )
    if not script.ok:
        return _fail(name, f"{script.name} {script.describe_failure()}", report), [
            report
        ]
    return 0, [report]


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
                repo, index, [(p, None) for p in gitutil.working_paths(repo)]
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
    code, reports = record(name, command, work, src, env)
    if code:
        return code

    samples = Samples()
    for report in reports:
        text = report.read_text(errors="replace")
        samples.merge(
            parse_sample(text, exclude=SAMPLE_HOLD_FRAMES)
            if darwin
            else parse_perf_script(text)
        )
    header = [
        f"profile {name}: {config.label}, modules {modules}",
        f"  tree      {perfcompare.current_label(opts.rev, commit, tree, bench)}",
        f"  recording {reports[0].parent} ({len(reports)} report(s))",
    ]
    lines = header + render(samples, opts.top)
    if not samples.total:
        lines.append(
            "  no samples: the run ended before the profiler "
            "attached (rows need a few milliseconds or more)"
        )
    out = "\n".join(lines) + "\n"
    (work / "result.txt").write_text(out)
    sys.stdout.write(out)
    return 0 if samples.total else 1

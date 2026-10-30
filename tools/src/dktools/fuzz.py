"""``dktools fuzz``: run the libFuzzer targets in ``src/fuzz``.

Usage::

    dktools fuzz NAME [TARGET ...] [--seconds N] [--root DIR] [-j N]

Exports HEAD plus the working tree to ROOT/fuzz/NAME/src, builds the
``fuzz`` preset (clang 22, ASan and UBSan, the library instrumented for
libFuzzer) with its ``datakit-test`` and ``datakit-fuzz`` targets, and
writes seeds: the persistence contract (``datakit-test test
persistContract`` with ``DK_FUZZ_SEEDS``) writes every registered type's
snapshots, op logs and persisted files in the inputs the persist
targets read.  Then each target (every built ``fuzz*`` program, or the
TARGETs named) runs for N seconds (default 60), one after another,
over its corpus (ROOT/fuzz/NAME/corpus/TARGET, kept from run to run, so
a later run with the same NAME starts from what earlier runs found) and
its seeds.

A target stops at its first finding (a crash, a sanitizer report, a
leak, a timeout, running out of memory, or a broken contract the target
asserts); the input is written under ROOT/fuzz/NAME/artifacts/TARGET,
and running the target's program on that file reproduces it.

Exits 0 when every target ran its time with no finding, 1 when one
found something or the build failed, 2 on an input error.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from . import gitutil, perfcompare, procs, steps, toolchain
from .gitutil import ToolError

#: The preset that builds the targets
PRESET = "fuzz"

#: The test that writes the persist seeds, and the targets it writes for
SEED_TEST = "persistContract"
SEEDED = ("fuzzPersistRestore", "fuzzPersistApplyOp", "fuzzPersistRecover")

#: One input running longer than this is a finding (libFuzzer -timeout)
INPUT_TIMEOUT_SECONDS = 10

#: The process's resident memory past this is a finding (libFuzzer
#: -rss_limit_mb).  ASan holds freed memory in a quarantine (256 MB) and
#: one input may peak near 500 MB, so libFuzzer's 2048 default trips on a
#: long run's allocator caching rather than on any one input.
RSS_LIMIT_MB = 4096

#: One allocation past this is a finding (libFuzzer -malloc_limit_mb)
MALLOC_LIMIT_MB = 2048

#: The program's run may take this much past its -max_total_time
#: (start-up reading the corpus, the final merge of its statistics)
SLACK_SECONDS = 300.0

_RUNS = re.compile(r"stat::number_of_executed_units:\s*(\d+)")
_FINDING = ("crash-", "leak-", "timeout-", "oom-", "slow-unit-")


@dataclass
class FuzzOptions:
    name: str
    targets: tuple[str, ...]
    seconds: int
    compiler: toolchain.Compiler
    root: Path | None
    jobs: int | None


@dataclass
class TargetResult:
    name: str
    ok: bool
    runs: int | None
    corpus: int
    finding: Path | None
    log: Path

    def line(self, program: Path) -> str:
        runs = f"{self.runs} runs" if self.runs is not None else "runs unknown"
        if self.ok:
            return f"  {self.name:<22} ok ({runs}, corpus {self.corpus})"
        if self.finding:
            return (
                f"  {self.name:<22} FOUND {self.finding.name} ({runs}); "
                f"reproduce: {program} {self.finding}"
            )
        return f"  {self.name:<22} FAILED with no input written; see {self.log}"


def executed_runs(log_text: str) -> int | None:
    """The executed-input count libFuzzer's final statistics report"""
    found = _RUNS.findall(log_text)
    return int(found[-1]) if found else None


def findings(artifacts: Path) -> list[Path]:
    """The inputs libFuzzer wrote under 'artifacts', newest last"""
    if not artifacts.is_dir():
        return []
    hits = [p for p in artifacts.iterdir() if p.name.startswith(_FINDING)]
    return sorted(hits, key=lambda p: p.stat().st_mtime)


def built_targets(bindir: Path) -> list[str]:
    """The fuzz target programs a fuzz build made"""
    return sorted(
        p.name
        for p in (bindir / "src").glob("fuzz*")
        if p.is_file() and p.suffix == "" and p.stat().st_mode & 0o111
    )


def fuzz_command(
    program: Path, corpus: Path, seeds: Path, artifacts: Path, seconds: int
) -> list[str]:
    """The libFuzzer argv: new inputs go to 'corpus', 'seeds' is read too"""
    cmd = [
        str(program),
        f"-max_total_time={seconds}",
        f"-timeout={INPUT_TIMEOUT_SECONDS}",
        f"-rss_limit_mb={RSS_LIMIT_MB}",
        f"-malloc_limit_mb={MALLOC_LIMIT_MB}",
        f"-artifact_prefix={artifacts}/",
        "-print_final_stats=1",
        str(corpus),
    ]
    if seeds.is_dir():
        cmd.append(str(seeds))
    return cmd


def run_target(name: str, program: Path, work: Path, seconds: int) -> TargetResult:
    corpus = work / "corpus" / name
    corpus.mkdir(parents=True, exist_ok=True)
    artifacts = work / "artifacts" / name
    artifacts.mkdir(parents=True, exist_ok=True)
    before = set(findings(artifacts))
    log = work / "logs" / f"{name}.log"
    step = steps.run_logged(
        name,
        fuzz_command(program, corpus, work / "seeds" / name, artifacts, seconds),
        log,
        cwd=work,
        timeout=seconds + SLACK_SECONDS,
    )
    new = [p for p in findings(artifacts) if p not in before]
    return TargetResult(
        name=name,
        ok=step.ok and not new,
        runs=executed_runs(log.read_text(errors="replace")) if log.is_file() else None,
        corpus=sum(1 for p in corpus.iterdir() if p.is_file()),
        finding=new[-1] if new else None,
        log=log,
    )


def run(repo: Path, opts: FuzzOptions) -> int:
    """Build, seed, fuzz; the exit code (ToolError: input error)"""
    name = gitutil.check_name(opts.name)
    if opts.seconds < 1:
        raise ToolError(f"--seconds must be at least 1 (got {opts.seconds})")
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    root = gitutil.scratch_root(opts.root, repo).resolve()
    config = toolchain.build_config(opts.compiler, PRESET)
    side, work = perfcompare.working_side(repo, root, "fuzz", name, config, keep=True)
    size = procs.sizing(opts.jobs)
    logs = gitutil.fresh_dir(work / "logs")

    configure = steps.run_logged(
        "fuzz configure",
        config.configure_command(side.src),
        logs / "configure.log",
        cwd=side.src,
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    build = steps.Step("fuzz build", logs / "build.log")
    if configure.ok:
        build = steps.run_logged(
            "fuzz build",
            config.build_command(
                side.src, size.build, ["datakit-test", "datakit-fuzz"], size.load
            ),
            build.log,
            cwd=side.src,
            timeout=steps.BUILD_TIMEOUT,
        )
    if not build.ok:
        failed = build if configure.ok else configure
        print(
            f"fuzz {name}: FAIL ({failed.name} {failed.describe_failure()}, "
            f"see {failed.log})"
        )
        return 1

    bindir = config.binary_dir(side.src)
    available = built_targets(bindir)
    unknown = [t for t in opts.targets if t not in available]
    if unknown:
        raise ToolError(
            f"no fuzz target {', '.join(unknown)} (built: {', '.join(available)})"
        )
    chosen = list(opts.targets) or available

    seeds = gitutil.fresh_dir(work / "seeds")
    for target in SEEDED:
        (seeds / target).mkdir()
    seeding = steps.run_logged(
        "fuzz seeds",
        [str(side.binary), "test", SEED_TEST],
        logs / "seeds.log",
        cwd=side.src,
        env={"DK_FUZZ_SEEDS": str(seeds)},
        timeout=steps.CTEST_TIMEOUT,
    )
    if not seeding.ok:
        print(
            f"fuzz {name}: FAIL (seeds {seeding.describe_failure()}, see {seeding.log})"
        )
        return 1
    seeded = sum(1 for t in SEEDED for p in (seeds / t).iterdir())

    print(
        f"fuzz {name}: {len(chosen)} target(s), {opts.seconds} s each, "
        f"{seeded} persist seeds"
    )
    results = []
    for target in chosen:
        result = run_target(target, bindir / "src" / target, work, opts.seconds)
        results.append(result)
        print(result.line(bindir / "src" / target), flush=True)
    found = [r for r in results if not r.ok]
    print(
        f"fuzz {name}: {'PASS' if not found else 'FOUND'} "
        f"({len(results) - len(found)} of {len(results)} clean)"
    )
    return 0 if not found else 1

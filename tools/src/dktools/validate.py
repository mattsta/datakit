"""The full build-and-test matrix on an exported revision.

Usage::

    dktools validate [--rev REV] [--compilers clang22,gcc15]
        [--presets dev,debug] [--serial REGEX] [--skip REGEX]
        [--debug-skip REGEX] [--leak-base BASE] [--root DIR] [-j N]

REV (default HEAD) is exported with ``git archive`` into
``ROOT/validate/<short hash>/src``; uncommitted work never takes part.
For each (compiler, preset) of the matrix (see
``toolchain.matrix``: gcc15 builds ``release``) it runs a clean
configure, an all-targets build, then ctest twice: every test outside
the serial and skip sets in parallel, then the serial set one test at
a time.  Tests matching ``--debug-skip`` (default ``multimap`` and
``multiFenwick``, too slow under Debug with sanitizers) are also
skipped in debug builds, and the output says so.

After the matrix it scans the test logs for UBSan ("runtime error:")
and ASan/LSan reports, runs ``allocaudit check --constructors`` on the
exported tree, and runs the leakage gate over the added lines of
``merge-base(BASE, REV)..REV`` (BASE default ``origin/main``; an empty
``--leak-base`` skips it).

Writes a markdown summary to ``ROOT/validate/<short hash>/summary.md``
and prints it, with compiler warnings grouped by file and split into
datakit ``src/``, ``deps/`` and other.  The macOS "no debug symbols in
executable" linker notes are counted apart and are not warnings.
Exits 0 only when every step passed with no warning, sanitizer report
or hard leak hit; 1 on any failure; 2 on an input error.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import steps, gitutil, leakcheck, toolchain
from .steps import CtestResult, SanitizerHit, Step, Warning_
from .gitutil import ToolError

#: Timing-sensitive tests, run one at a time so the parallel set does
#: not compete with them for CPUs.  dkTestNonceLogEvictionPerf times
#: 20000 evictions with the monotonic clock and fails above 5000 ns
#: each, so a preempted run can fail with nothing wrong.  Everything
#: else runs in parallel: the persist tests (the *P wrappers, persist,
#: persistCtx) each take a private mkdtemp scratch directory, and
#: dkTestKeyringGetRawCopyMigrationCluster asserts its thread race
#: deterministically.
DEFAULT_SERIAL = r"dkTestNonceLogEvictionPerf"
#: Tests validate never runs: vendored suites and benchmarks.
DEFAULT_SKIP = r"varint-.*|perfRegression|compressionBench"
#: Tests too slow for a Debug + sanitizer build.
DEFAULT_DEBUG_SKIP = r"multimap|multiFenwick"
DEFAULT_LEAK_BASE = "origin/main"


@dataclass
class ValidateOptions:
    rev: str = "HEAD"
    compilers: list[toolchain.Compiler] = field(default_factory=list)
    presets: list[str] = field(default_factory=lambda: ["dev", "debug"])
    serial: str = DEFAULT_SERIAL
    skip: str = DEFAULT_SKIP
    debug_skip: str = DEFAULT_DEBUG_SKIP
    leak_base: str = DEFAULT_LEAK_BASE
    root: Path | None = None
    jobs: int | None = None


@dataclass
class ConfigResult:
    config: toolchain.BuildConfig
    configure: Step
    build: Step
    ctest: Step
    serial: Step
    ctest_result: CtestResult
    serial_result: CtestResult
    warnings: list[Warning_]
    warning_lines: int
    linker_notes: int
    sanitizer: list[SanitizerHit]
    test_outputs: list[Path]
    extra_skip: str

    @property
    def ok(self) -> bool:
        return (
            self.configure.ok
            and self.build.ok
            and self.ctest.ok
            and self.serial.ok
            and self.warning_lines == 0
            and not self.sanitizer
        )


def _run_config(
    config: toolchain.BuildConfig,
    src: Path,
    logs: Path,
    opts: ValidateOptions,
    jobs: int,
) -> ConfigResult:
    bindir = config.binary_dir(src)
    gitutil.fresh_dir(bindir)
    gitutil.fresh_dir(logs)
    tag = config.label
    configure = steps.run_logged(
        f"{tag} configure",
        config.configure_command(src),
        logs / "configure.log",
        cwd=src,
    )
    build = Step("build", logs / "build.log")
    if configure.ok:
        build = steps.run_logged(
            f"{tag} build", config.build_command(src, jobs, None), build.log, cwd=src
        )
    extra_skip = opts.debug_skip if config.preset == "debug" else ""
    ctest = Step("ctest", logs / "ctest.log")
    serial = Step("ctest-serial", logs / "ctest-serial.log")
    outputs: list[Path] = []
    if build.ok:
        ctest = steps.run_logged(
            f"{tag} ctest",
            steps.ctest_command(
                bindir, jobs, None, exclude=[opts.serial, opts.skip, extra_skip]
            ),
            ctest.log,
            cwd=bindir,
        )
        kept = steps.keep_test_log(bindir, logs / "ctest-output.log")
        if kept:
            outputs.append(kept)
        if opts.serial:
            serial = steps.run_logged(
                f"{tag} ctest-serial",
                steps.ctest_command(
                    bindir, 1, opts.serial, exclude=[opts.skip, extra_skip]
                ),
                serial.log,
                cwd=bindir,
            )
            kept = steps.keep_test_log(bindir, logs / "ctest-serial-output.log")
            if kept:
                outputs.append(kept)
        else:
            serial.code = 0
    return ConfigResult(
        config=config,
        configure=configure,
        build=build,
        ctest=ctest,
        serial=serial,
        ctest_result=steps.parse_ctest(ctest.log),
        serial_result=steps.parse_ctest(serial.log),
        warnings=steps.warnings(build.log, src),
        warning_lines=steps.count_warning_lines(build.log),
        linker_notes=steps.count_linker_notes(build.log),
        sanitizer=steps.sanitizer_reports(outputs),
        test_outputs=outputs,
        extra_skip=extra_skip,
    )


def _step_cell(step: Step, result: CtestResult | None = None) -> str:
    if step.code is None:
        return "not run"
    word = "ok" if step.ok else f"FAIL ({step.code})"
    if result is not None and result.summary:
        return f"{word}: {result.summary}"
    return word


def _names(regex: str) -> str:
    return ", ".join(part for part in regex.split("|") if part)


def render(
    commit: str,
    subject: str,
    results: list[ConfigResult],
    opts: ValidateOptions,
    alloc: Step,
    leak: tuple[str, list[leakcheck.Hit]] | None,
    passed: bool,
) -> str:
    """The markdown summary."""
    md = [
        f"# validate {commit[:10]}: {'PASS' if passed else 'FAIL'}",
        "",
        f"Revision `{commit}`: {subject}",
        "",
        "| config | configure | build | warnings | ctest | serial | sanitizer |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in results:
        md.append(
            f"| {r.config.label} | {_step_cell(r.configure)} | {_step_cell(r.build)} "
            f"| {len(r.warnings)} distinct ({r.warning_lines} lines; "
            f"{r.linker_notes} linker notes) "
            f"| {_step_cell(r.ctest, r.ctest_result)} "
            f"| {_step_cell(r.serial, r.serial_result)} "
            f"| {len(r.sanitizer)} |"
        )
    md += ["", "## Skipped tests", ""]
    md.append(f"- every config: {_names(opts.skip) or '(none)'}")
    for r in results:
        if r.extra_skip:
            md.append(
                f"- {r.config.label}: also {_names(r.extra_skip)} "
                "(too slow under Debug + sanitizers; `--debug-skip ''` runs them)"
            )
        not_run = r.ctest_result.not_run + r.serial_result.not_run
        if not_run:
            md.append(f"- {r.config.label}: did not run: {', '.join(not_run)}")
    md.append(f"- serial set (one at a time): {_names(opts.serial) or '(none)'}")

    md += ["", "## Failed tests", ""]
    any_failed = False
    for r in results:
        failed = r.ctest_result.failed + r.serial_result.failed
        for missing in (r.ctest_result.missing, r.serial_result.missing):
            failed += [f"{name} (executable not built)" for name in missing]
        if failed:
            any_failed = True
            md.append(f"- {r.config.label}: {', '.join(failed)}")
    if not any_failed:
        md.append("none")

    md += ["", "## Warnings", ""]
    for r in results:
        groups = steps.group_warnings(r.warnings)
        md.append(f"### {r.config.label}")
        md.append("")
        for area, title in (
            ("src", "datakit src/"),
            ("deps", "deps/"),
            ("other", "other"),
        ):
            files = groups[area]
            md.append(f"- {title}: {sum(files.values())}")
            for path in sorted(files):
                md.append(f"  - `{path}`: {files[path]}")
        md.append("")

    md += ["## Sanitizer reports", ""]
    hits = [h for r in results for h in r.sanitizer]
    if not hits:
        md.append("none")
    for h in hits[:40]:
        md.append(
            f"- `{h.log.parent.name}/{h.log.name}:{h.line}` "
            f"{h.test or '(unknown test)'}: {h.text}"
        )
    if len(hits) > 40:
        md.append(f"- ... and {len(hits) - 40} more")

    md += ["", "## Allocation ownership", ""]
    md.append(f"{_step_cell(alloc)}: {steps.last_line(alloc.log)}")

    md += ["", "## Leakage gate", ""]
    if leak is None:
        md.append("skipped (empty --leak-base)")
    else:
        rev_range, leak_hits = leak
        hard = [h for h in leak_hits if h.term.hard]
        review = [h for h in leak_hits if not h.term.hard]
        md.append(
            f"range `{rev_range}`: {len(hard)} hard hit(s), {len(review)} review hit(s)"
        )
        for h in hard[:40]:
            md.append(f"- {leakcheck.format_hit(h)}")
        if len(hard) > 40:
            md.append(f"- ... and {len(hard) - 40} more (see leakcheck.log)")

    md += ["", "## Logs", ""]
    for r in results:
        for step in (r.configure, r.build, r.ctest, r.serial):
            if step.log.exists():
                md.append(f"- {step.log}")
        for out in r.test_outputs:
            md.append(f"- {out}")
    md.append(f"- {alloc.log}")
    return "\n".join(md) + "\n"


def run(repo: Path, opts: ValidateOptions) -> int:
    """Run the matrix; return the exit code (ToolError: input error)."""
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    if not opts.compilers:
        raise ToolError("validate needs at least one compiler")
    jobs = opts.jobs or os.cpu_count() or 4
    commit = gitutil.resolve_commit(repo, opts.rev)
    subject = gitutil.git_text(repo, "log", "-1", "--format=%s", commit)
    configs = toolchain.matrix(opts.compilers, opts.presets)
    for config in configs:
        config.compiler.check()
    leak_range = None
    if opts.leak_base:
        base = gitutil.resolve_commit(repo, opts.leak_base)
        merge_base = gitutil.git_text(repo, "merge-base", base, commit)
        leak_range = f"{merge_base}..{commit}"

    root = gitutil.scratch_root(opts.root).resolve()
    work = gitutil.fresh_dir(root / "validate" / commit[:10])
    src = work / "src"
    gitutil.export_tree(repo, commit, src)
    for config in configs:
        config.check_presets_file(src)

    results = [_run_config(c, src, work / c.label, opts, jobs) for c in configs]

    alloc = Step("allocaudit", work / "alloc.log")
    try:
        uv = gitutil.require_program("uv")
    except ToolError as exc:
        alloc.log.write_text(f"{exc}\n")
        alloc.code = 127
    else:
        alloc = steps.run_logged(
            "allocaudit",
            steps.allocaudit_command(uv, src),
            alloc.log,
            cwd=src / "tools",
        )

    leak = None
    leak_ok = True
    if leak_range is not None:
        hits, scanned = leakcheck.check(repo, "range", leak_range)
        with open(work / "leakcheck.log", "w") as log:
            leak_ok = leakcheck.report(hits, scanned, out=log) == 0
        leak = (leak_range, hits)

    passed = all(r.ok for r in results) and alloc.ok and leak_ok
    text = render(commit, subject, results, opts, alloc, leak, passed)
    summary = work / "summary.md"
    summary.write_text(text)
    sys.stdout.write(text)
    print(f"summary: {summary}")
    return 0 if passed else 1

"""The full build-and-test matrix on an exported revision.

Usage::

    dktools validate [--rev REV] [--compilers clang22,gcc15]
        [--presets dev,debug] [--serial REGEX] [--skip REGEX]
        [--debug-skip REGEX] [--leak-base BASE] [--root DIR] [-j N]

REV (default HEAD) is exported with ``git archive`` into
``ROOT/validate/<short hash>-<configs>/src`` (``<configs>`` joins the
matrix's labels, so matrices on one revision keep separate logs);
uncommitted work never takes part.
For each (compiler, preset) of the matrix (see
``toolchain.matrix``: gcc15 builds ``release``) it runs a clean
configure, an all-targets build, then ctest twice: every test outside
the serial and skip sets in parallel, then the serial set one test at
a time.  Two tiers: the default quick tier (iterative work) also skips
the slow tests, ``--slow`` in every build and ``--debug-slow`` in debug
and tsan builds; ``--full`` (milestones) runs them.  In every tier,
tests a preset cannot judge are skipped there: ``--debug-skip`` (default
none) and ``--tsan-skip`` (default ``dkTestNonceLogEvictionPerf``, a
wall-clock limit TSan breaks).  The summary names every skipped test,
why, and how to run it.

After the matrix it scans the test logs for UBSan ("runtime error:"),
ASan/LSan and TSan ("WARNING: ThreadSanitizer:") reports, runs ``allocaudit check --constructors``,
``atomicaudit check`` and ``outputaudit check`` on the exported tree, checks
that every file of the tree is formatted (``dktools.formatting``; it rewrites
nothing: ``dktools format write`` does), and runs the leakage gate
over the added lines of
``merge-base(BASE, REV)..REV`` (BASE default ``origin/main``; an empty
``--leak-base`` skips it).

Writes a markdown summary to
``ROOT/validate/<short hash>-<configs>/summary.md``
and prints it, with compiler warnings grouped by file and split into
datakit ``src/``, ``deps/`` and other.  The macOS "no debug symbols in
executable" linker notes are counted apart and are not warnings.
Exits 0 only when every step passed with no warning, sanitizer report
or hard leak hit; 1 on any failure; 2 on an input error.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, formatting, gitutil, leakcheck, procs, steps, toolchain
from .gitutil import ToolError
from .steps import CtestResult, SanitizerHit, Step, Warning_

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
DEFAULT_SKIP = r"varint-.*|perfRegression|compressionBench|fenwickBench"
#: Tests a Debug + sanitizer build cannot judge, in any tier (none: every
#: suite finishes there since the in-test benchmarks became smoke-size).
DEFAULT_DEBUG_SKIP = r""
#: Tests a ThreadSanitizer build cannot judge, in any tier: wall-clock
#: limits that TSan's instrumentation breaks.
DEFAULT_TSAN_SKIP = r"dkTestNonceLogEvictionPerf"
#: The quick tier (the default; --full is the milestone tier) also skips
#: the slow tests: in every build, those over ~40 s under the dev preset;
#: in debug and tsan builds, also those over ~60 s there.  None at
#: present: the suites that were here run alone in about 10 s or less on
#: dev (measured 2026-10-21, after their oversized loops became boundary
#: tests).  A suite that must stay long goes here rather than slowing
#: every quick run.
#: verify always runs exactly the tests it is given.
DEFAULT_SLOW = r""
DEFAULT_DEBUG_SLOW = r""
DEFAULT_LEAK_BASE = "origin/main"


@dataclass
class ValidateOptions:
    rev: str = "HEAD"
    compilers: list[toolchain.Compiler] = field(default_factory=list)
    presets: list[str] = field(default_factory=lambda: ["dev", "debug"])
    serial: str = DEFAULT_SERIAL
    skip: str = DEFAULT_SKIP
    debug_skip: str = DEFAULT_DEBUG_SKIP
    tsan_skip: str = DEFAULT_TSAN_SKIP
    slow: str = DEFAULT_SLOW
    debug_slow: str = DEFAULT_DEBUG_SLOW
    full: bool = False
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


def serial_for(serial: str, skips: list[str]) -> str:
    """The serial set this build runs: its names that no skip regex names
    literally.  Empty when every serial test is skipped here (the serial
    step is then not run, rather than failing on no tests); a name that
    matches no test at all still fails the step, as a typo must."""
    skipped = {name for regex in skips for name in regex.split("|") if name}
    return "|".join(n for n in serial.split("|") if n and n not in skipped)


def preset_skip(opts: ValidateOptions, preset: str) -> str:
    """The ctest name regex a `preset` build skips on top of --skip: the
    tests the preset cannot judge (always), and in the quick tier (the
    default; --full is the milestone tier) the slow ones."""
    parts = [{"debug": opts.debug_skip, "tsan": opts.tsan_skip}.get(preset, "")]
    if not opts.full:
        parts.append(opts.slow)
        if preset in ("debug", "tsan"):
            parts.append(opts.debug_slow)
    return "|".join(p for p in parts if p)


def _run_config(
    config: toolchain.BuildConfig,
    src: Path,
    logs: Path,
    opts: ValidateOptions,
    size: procs.Sizing,
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
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    build = Step("build", logs / "build.log")
    if configure.ok:
        build = steps.run_logged(
            f"{tag} build",
            config.build_command(src, size.build, None, size.load),
            build.log,
            cwd=src,
            timeout=steps.BUILD_TIMEOUT,
        )
    extra_skip = preset_skip(opts, config.preset)
    ctest = Step("ctest", logs / "ctest.log")
    serial = Step("ctest-serial", logs / "ctest-serial.log")
    outputs: list[Path] = []
    if build.ok:
        ctest = steps.run_logged(
            f"{tag} ctest",
            steps.ctest_command(
                bindir,
                size.test,
                None,
                exclude=[opts.serial, opts.skip, extra_skip],
                load=size.load,
            ),
            ctest.log,
            cwd=bindir,
            timeout=steps.CTEST_TIMEOUT,
        )
        kept = steps.keep_test_log(bindir, logs / "ctest-output.log")
        if kept:
            outputs.append(kept)
        serial_here = serial_for(opts.serial, [opts.skip, extra_skip])
        if serial_here:
            serial = steps.run_logged(
                f"{tag} ctest-serial",
                steps.ctest_command(
                    bindir, 1, serial_here, exclude=[opts.skip, extra_skip]
                ),
                serial.log,
                cwd=bindir,
                timeout=steps.CTEST_TIMEOUT,
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
    if step.ok:
        word = "ok"
    elif step.timed_out:
        word = "FAIL (timed out)"
    else:
        word = f"FAIL ({step.code})"
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
    atomic: Step,
    output: Step,
    leak: tuple[str, list[leakcheck.Hit]] | None,
    passed: bool,
    fmt: tuple[formatting.Report, Path],
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
    md.append(
        "- tier: full (milestone: slow tests run)"
        if opts.full
        else "- tier: quick (iterative: slow tests skipped; `--full` runs them)"
    )
    md.append(f"- every config: {_names(opts.skip) or '(none)'}")
    for r in results:
        preset = r.config.preset
        cannot = {"debug": opts.debug_skip, "tsan": opts.tsan_skip}.get(preset, "")
        if cannot:
            flag = "--tsan-skip" if preset == "tsan" else "--debug-skip"
            md.append(
                f"- {r.config.label}: cannot be judged here: {_names(cannot)} "
                f"(`{flag} ''` runs them)"
            )
        slow = [] if opts.full else [opts.slow]
        if not opts.full and preset in ("debug", "tsan"):
            slow.append(opts.debug_slow)
        slow_regex = "|".join(s for s in slow if s)
        if slow_regex:
            md.append(
                f"- {r.config.label}: slow, skipped in the quick tier: "
                f"{_names(slow_regex)}"
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

    md += ["", "## Atomic ordering", ""]
    md.append(f"{_step_cell(atomic)}: {steps.last_line(atomic.log)}")

    md += ["", "## Library output", ""]
    md.append(f"{_step_cell(output)}: {steps.last_line(output.log)}")

    md += ["", "## Formatting", ""]
    md.append(formatting.gate_line(fmt[0]))
    md += [f"- {line}" for line in fmt[0].lines("would change")]

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
    md.append(f"- {atomic.log}")
    md.append(f"- {output.log}")
    md.append(f"- {fmt[1]}")
    return "\n".join(md) + "\n"


def work_dir_name(commit: str, configs) -> str:
    """The work directory of one matrix on one revision: the short hash
    and the configurations, so a second matrix on the same revision (a
    TSan run after the default one) keeps the first one's logs."""
    return f"{commit[:10]}-{'+'.join(c.label for c in configs)}"


def run(repo: Path, opts: ValidateOptions) -> int:
    """Run the matrix; return the exit code (ToolError: input error)."""
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    if not opts.compilers:
        raise ToolError("validate needs at least one compiler")
    size = procs.sizing(opts.jobs)
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

    root = gitutil.scratch_root(opts.root, repo).resolve()
    work = buildstore.fresh_build(
        repo, root, "validate", work_dir_name(commit, configs), commit, "validate"
    )
    src = work / "src"
    gitutil.export_tree(repo, commit, src)
    for config in configs:
        config.check_presets_file(src)
    fmt = formatting.check(src, formatting.tree_files(repo, commit))
    formatting.write_log(fmt, work / "format.log")

    results = [_run_config(c, src, work / c.label, opts, size) for c in configs]

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
            timeout=steps.ALLOCAUDIT_TIMEOUT,
        )

    atomic = steps.run_tree_gate(
        "atomicaudit",
        steps.atomicaudit_command,
        work / "atomic.log",
        src,
        steps.ATOMICAUDIT_TIMEOUT,
    )
    output = steps.run_tree_gate(
        "outputaudit",
        steps.outputaudit_command,
        work / "output.log",
        src,
        steps.OUTPUTAUDIT_TIMEOUT,
    )

    leak = None
    leak_ok = True
    if leak_range is not None:
        hits, scanned = leakcheck.check(repo, "range", leak_range)
        with (work / "leakcheck.log").open("w") as log:
            leak_ok = leakcheck.report(hits, scanned, out=log) == 0
        leak = (leak_range, hits)

    passed = (
        all(r.ok for r in results)
        and alloc.ok
        and atomic.ok
        and output.ok
        and leak_ok
        and fmt.clean()
    )
    text = render(
        commit,
        subject,
        results,
        opts,
        alloc,
        atomic,
        output,
        leak,
        passed,
        (fmt, work / "format.log"),
    )
    summary = work / "summary.md"
    summary.write_text(text)
    sys.stdout.write(text)
    print(f"summary: {summary}")
    return 0 if passed else 1

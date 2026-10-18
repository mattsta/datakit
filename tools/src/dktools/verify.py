"""Build HEAD plus exactly the named paths, in isolation, and test it.

Usage::

    dktools verify NAME --tests REGEX [--serial REGEX]
        [--compiler clang22|gcc15] [--preset dev|debug]
        [--target T ...] [--root DIR] [-j N] PATH[=FILE]...

The tree under test is HEAD with each PATH replaced by its working-tree
content (or removed, when PATH is deleted in the working tree), or
with FILE's content staged at PATH for ``PATH=FILE``.  Nothing else
from the working tree leaks in, and the real index is never touched:
the tree is written through a private GIT_INDEX_FILE, exported with
``git archive`` into ``ROOT/verify/NAME/src``, then configured and
built there with the chosen compiler and preset (see
``dktools.toolchain``).

``--tests`` names the ctest tests run in parallel (``-j N``);
``--serial`` names tests run one at a time, such as the ``*P`` tests
that share fixed temp paths, and those are excluded from the parallel
run.  Both are whole-name regexes, and a regex that matches no test is
a failure.  ``allocaudit check --constructors`` runs on the tree with
the tree's own dktools.

Prints a result block (tree hash, build, warning and linker-note
counts, ctest, sanitizer and allocaudit results, failing test names,
log paths) and also writes it to ``ROOT/verify/NAME/result.txt``.
Exits 0 only when every step passed with zero warning lines and no
sanitizer report, 1 otherwise, and 2 on an input error.  The macOS
"no debug symbols in executable" lines are linker notes, not warnings
(``steps.is_linker_note``): they are counted on their own and never
fail the run.

Each sanitizer report (UBSan ``runtime error:``, ASan/LSan/TSan
``ERROR:``) prints as one ``report`` line: the test whose ctest output
section holds it, the report's first line unshortened, and its
``LOG:LINE``.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import steps, gitutil, toolchain
from .steps import Step
from .gitutil import ToolError


@dataclass
class VerifyOptions:
    name: str
    specs: list[str]
    tests: str | None = None
    serial: str | None = None
    compiler: toolchain.Compiler = field(
        default_factory=lambda: toolchain.resolve_compiler(toolchain.DEFAULT_COMPILER)
    )
    preset: str | None = None
    targets: list[str] | None = None
    root: Path | None = None
    jobs: int | None = None


def parse_specs(specs: list[str]) -> list[tuple[str, Path | None]]:
    """``PATH`` or ``PATH=FILE`` arguments as (repo path, FILE or None)."""
    if not specs:
        raise ToolError("verify needs at least one PATH")
    parsed: list[tuple[str, Path | None]] = []
    for spec in specs:
        path, sep, file = spec.partition("=")
        if sep and not file:
            raise ToolError(f"{spec}: PATH=FILE needs a FILE")
        parsed.append((gitutil.repo_path(path), Path(file) if sep else None))
    return parsed


def _ctest_line(step: Step, result: steps.CtestResult) -> str:
    if step.code is None:
        return "not run"
    detail = result.summary or ("no tests matched" if result.no_tests else "no summary")
    return f"{'ok' if step.ok else 'FAILED'} ({detail})"


def _sanitizer_line(hits: list[steps.SanitizerHit]) -> str:
    if not hits:
        return "ok, no reports"
    tests = steps.tests_with_reports(hits)
    return f"{len(hits)} report(s) from {len(tests)} test(s): {' '.join(tests)}"


def run(repo: Path, opts: VerifyOptions) -> int:
    """Run the verification; return the exit code (ToolError: input)."""
    name = gitutil.check_name(opts.name)
    if not opts.tests and not opts.serial:
        raise ToolError("verify needs --tests and/or --serial")
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    specs = parse_specs(opts.specs)
    config = toolchain.build_config(opts.compiler, opts.preset)
    jobs = opts.jobs or os.cpu_count() or 4
    targets = opts.targets or list(toolchain.PRESET_TARGETS)

    root = gitutil.scratch_root(opts.root).resolve()
    work = gitutil.fresh_dir(root / "verify" / name)
    src = work / "src"

    head = gitutil.git_text(repo, "rev-parse", "--short", "HEAD")
    tree = gitutil.build_tree(repo, work / "index", specs)
    gitutil.export_tree(repo, tree, src)
    config.compiler.check()
    config.check_presets_file(src)
    bindir = config.binary_dir(src)

    configure = steps.run_logged(
        "configure", config.configure_command(src), work / "configure.log", cwd=src
    )
    build = Step("build", work / "build.log")
    if configure.ok:
        build = steps.run_logged(
            "build", config.build_command(src, jobs, targets), build.log, cwd=src
        )
    warn = steps.count_warning_lines(build.log)
    notes = steps.count_linker_notes(build.log)

    ctest = Step("ctest", work / "ctest.log")
    serial = Step("ctest-serial", work / "ctest-serial.log")
    test_outputs: list[Path] = []
    if build.ok and opts.tests:
        ctest = steps.run_logged(
            "ctest",
            steps.ctest_command(bindir, jobs, opts.tests, exclude=[opts.serial or ""]),
            ctest.log,
            cwd=bindir,
        )
        kept = steps.keep_test_log(bindir, work / "ctest-output.log")
        if kept:
            test_outputs.append(kept)
    if build.ok and opts.serial:
        serial = steps.run_logged(
            "ctest-serial",
            steps.ctest_command(bindir, 1, opts.serial),
            serial.log,
            cwd=bindir,
        )
        kept = steps.keep_test_log(bindir, work / "ctest-serial-output.log")
        if kept:
            test_outputs.append(kept)
    ctest_result = steps.parse_ctest(ctest.log)
    serial_result = steps.parse_ctest(serial.log)
    sanitizer = steps.sanitizer_reports(test_outputs)

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

    failed = ctest_result.failed + serial_result.failed
    passed = (
        configure.ok
        and build.ok
        and warn == 0
        and (ctest.ok or not opts.tests)
        and (serial.ok or not opts.serial)
        and not sanitizer
        and alloc.ok
    )

    counts = f"{warn} warning lines, {notes} linker notes"
    if not configure.ok:
        build_line = f"FAILED (configure exit {configure.code})"
    elif not build.ok:
        build_line = f"FAILED (exit {build.code}), {counts}"
    else:
        build_line = f"ok, {counts}"
    lines = [
        f"verify {name}: {'PASS' if passed else 'FAIL'}",
        f"  tree      {tree} (HEAD {head} + {len(specs)} path(s))",
        f"  config    {config.label}",
        f"  build     {build_line}",
        f"  ctest     {_ctest_line(ctest, ctest_result) if opts.tests else '-'}",
        f"  serial    {_ctest_line(serial, serial_result) if opts.serial else '-'}",
        f"  sanitizer {_sanitizer_line(sanitizer)}",
        f"  alloc     {'ok' if alloc.ok else 'FAILED'} ({steps.last_line(alloc.log)})",
    ]
    if failed:
        lines.append(f"  failed    {' '.join(failed)}")
    not_run = ctest_result.not_run + serial_result.not_run
    if not_run:
        lines.append(f"  not run   {' '.join(not_run)}")
    for hit in sanitizer:
        lines.append(f"  report    {hit.describe()}")
    lines.append("  logs")
    for step in (configure, build, ctest, serial, alloc):
        if step.log.exists():
            lines.append(f"    {step.log}")
    for out in test_outputs:
        lines.append(f"    {out}")
    text = "\n".join(lines) + "\n"
    (work / "result.txt").write_text(text)
    sys.stdout.write(text)
    return 0 if passed else 1

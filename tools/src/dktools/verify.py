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
``--serial`` names tests run one at a time, such as the
timing-sensitive tests in ``validate.DEFAULT_SERIAL``, and those are
excluded from the parallel run.  Both are whole-name regexes, and a regex that matches no test is
a failure.  ``allocaudit check --constructors`` runs on the tree with
the tree's own dktools.

Without ``--target`` the build covers the four preset targets plus
every target the selected tests run: configure is asked for the CMake
File API codemodel (each target's artifacts), ``ctest
--show-only=json-v1`` names the selected tests, ``ctestCommands.cmake``
gives every test's command line (ctest's JSON omits the command of a
test whose executable is not built yet), and a test needs every
target whose artifact its command names.  So
``--tests 'varint-.*'`` builds deps/varint's test executables without
``--target all``.  ``--target`` builds exactly the listed targets.  A
test that still finds no executable (an explicit ``--target`` list
that leaves it out) is reported on its own ``missing`` line with the
targets to add, not as an ordinary failure.

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


@dataclass
class TargetPlan:
    """The build targets and why: the preset targets plus each target
    a selected test runs, or exactly the --target list."""

    targets: list[str]
    explicit: bool
    added: list[str] = field(default_factory=list)
    artifacts: dict[str, str] = field(default_factory=dict)

    def line(self) -> str:
        if self.explicit:
            return f"{' '.join(self.targets)} (--target)"
        base = [t for t in self.targets if t not in self.added]
        text = " ".join(base)
        if self.added:
            text += (
                f" + {len(self.added)} for the selected tests: {' '.join(self.added)}"
            )
        return text

    def target_of(self, exe: str) -> str | None:
        return self.artifacts.get(os.path.realpath(exe))


def _plan_targets(bindir: Path, work: Path, opts: VerifyOptions) -> TargetPlan:
    """After configure: the targets to build (see TargetPlan)."""
    artifacts = steps.target_artifacts(bindir)
    if opts.targets:
        return TargetPlan(list(opts.targets), True, artifacts=artifacts)
    plan = TargetPlan(list(toolchain.PRESET_TARGETS), False, artifacts=artifacts)
    include = "|".join(r for r in (opts.tests, opts.serial) if r)
    listing = steps.run_logged(
        "ctest-list",
        steps.list_tests_command(bindir, include),
        work / "tests.json",
        cwd=bindir,
    )
    commands_out = work / "test-commands.txt"
    steps.run_logged(
        "test-commands",
        steps.test_commands_command(bindir, commands_out),
        work / "test-commands.log",
        cwd=bindir,
    )
    commands = steps.parse_test_commands(commands_out)
    selected = {
        test: commands[test]
        for test in steps.parse_test_names(listing.log)
        if test in commands
    }
    needed = steps.targets_for_tests(selected, artifacts)
    for test_targets in needed.values():
        for target in test_targets:
            if target not in plan.targets:
                plan.targets.append(target)
                plan.added.append(target)
    return plan


def _missing_lines(
    missing: dict[str, str], plan: TargetPlan, bindir: Path
) -> list[str]:
    """The result lines for tests whose executable was never built."""
    if not missing:
        return []
    names: list[str] = []
    for exe in missing.values():
        target = plan.target_of(exe)
        if target is None:
            real = os.path.realpath(exe)
            top = os.path.realpath(bindir) + os.sep
            target = real[len(top) :] if real.startswith(top) else exe
        names.append(target)
    names = list(dict.fromkeys(names))
    return [
        f"  missing   {len(missing)} test(s) did not run, executable not built: "
        f"{' '.join(missing)}",
        f"  hint      not built: {' '.join(names)}; "
        "add them with --target, or use --target all",
    ]


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

    root = gitutil.scratch_root(opts.root).resolve()
    work = gitutil.fresh_dir(root / "verify" / name)
    src = work / "src"

    head = gitutil.git_text(repo, "rev-parse", "--short", "HEAD")
    tree = gitutil.build_tree(repo, work / "index", specs)
    gitutil.export_tree(repo, tree, src)
    config.compiler.check()
    config.check_presets_file(src)
    bindir = config.binary_dir(src)

    steps.request_codemodel(bindir)
    configure = steps.run_logged(
        "configure", config.configure_command(src), work / "configure.log", cwd=src
    )
    build = Step("build", work / "build.log")
    plan: TargetPlan | None = None
    if configure.ok:
        plan = _plan_targets(bindir, work, opts)
        build = steps.run_logged(
            "build", config.build_command(src, jobs, plan.targets), build.log, cwd=src
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
        f"  targets   {plan.line() if plan else '-'}",
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
    if plan is not None:
        missing = {**ctest_result.missing, **serial_result.missing}
        lines += _missing_lines(missing, plan, bindir)
    for hit in sanitizer:
        lines.append(f"  report    {hit.describe()}")
    lines.append("  logs")
    for log in (
        configure.log,
        work / "tests.json",
        work / "test-commands.log",
        build.log,
        ctest.log,
        serial.log,
        alloc.log,
    ):
        if log.exists():
            lines.append(f"    {log}")
    for out in test_outputs:
        lines.append(f"    {out}")
    text = "\n".join(lines) + "\n"
    (work / "result.txt").write_text(text)
    sys.stdout.write(text)
    return 0 if passed else 1

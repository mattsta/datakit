"""Build HEAD plus exactly the named paths, in isolation, and test it.

Usage::

    dktools verify NAME --tests REGEX [--serial REGEX]
        [--compiler clang22|gcc15] [--preset dev|debug]
        [--target T ...] [--root DIR] [-j N]
        (PATH[=FILE]... | --staged | --all-changed)

The tree under test is HEAD with each PATH replaced by its working-tree
content (or removed, when PATH is deleted in the working tree), or
with FILE's content staged at PATH for ``PATH=FILE``.  Instead of
PATHs, ``--staged`` takes exactly the current index (HEAD plus what
is staged), and ``--all-changed`` takes HEAD plus every tracked path
that differs in the working tree (modified, added to the index, or
deleted); untracked files never take part.  Either flag refuses (exit
2) when the result would be plain HEAD ("index matches HEAD", "no
tracked changes"): name an unchanged PATH to build HEAD on purpose.
A refused run, like any input error found while building the tree,
leaves an earlier result under NAME untouched.  Nothing else from the working tree leaks in,
and the real index is never touched: the tree is written through a
private GIT_INDEX_FILE (a copy of the index for ``--staged``),
exported with ``git archive`` into ``ROOT/verify/NAME/src``, then
configured and built there with the chosen compiler and preset (see
``dktools.toolchain``).  The result lists every path that differs
from HEAD, one ``changed`` line each.

``--tests`` names the ctest tests run in parallel (``-j N``);
``--serial`` names tests run one at a time, such as the
timing-sensitive tests in ``validate.DEFAULT_SERIAL``, and those are
excluded from the parallel run.  Both are whole-name regexes, and a
regex that matches no test is a failure.  ``allocaudit check --constructors``,
``atomicaudit check`` and ``outputaudit check`` run on the tree with the tree's
own dktools.

Formatting is part of the run (``dktools.formatting``): before the tree
is written, the working-tree files it takes are formatted in place
(every path with --all-changed, the named paths otherwise, nothing with
--staged), and the ``format`` line checks every file of the tree built,
so an unformatted file fails the run.

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
counts, ctest, sanitizer, allocaudit, atomicaudit, outputaudit and format results, failing test names,
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
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, formatting, gitutil, procs, steps, toolchain
from .gitutil import ToolError
from .steps import Step


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
    staged: bool = False  # the tree is exactly the current index
    all_changed: bool = False  # HEAD plus every changed tracked path


def parse_specs(specs: list[str]) -> list[tuple[str, Path | None]]:
    """``PATH`` or ``PATH=FILE`` arguments as (repo path, FILE or None)."""
    if not specs:
        raise ToolError("verify needs PATH arguments, --staged or --all-changed")
    parsed: list[tuple[str, Path | None]] = []
    for spec in specs:
        path, sep, file = spec.partition("=")
        if sep and not file:
            raise ToolError(f"{spec}: PATH=FILE needs a FILE")
        parsed.append((gitutil.repo_path(path), Path(file) if sep else None))
    return parsed


def check_source(opts: VerifyOptions) -> str:
    """Which tree the options select: "paths", "staged" or
    "all-changed"; ToolError when they conflict."""
    if opts.staged and opts.all_changed:
        raise ToolError("--staged and --all-changed are mutually exclusive")
    flag = "--staged" if opts.staged else "--all-changed" if opts.all_changed else ""
    if flag and opts.specs:
        raise ToolError(f"{flag} takes no PATH arguments (got {opts.specs[0]})")
    return flag.lstrip("-") or "paths"


def build_source_tree(
    repo: Path, index_file: Path, source: str, specs: list[tuple[str, Path | None]]
) -> tuple[str, list[tuple[str, str]]]:
    """The tree to verify and its (status, path) changes from HEAD.

    --staged and --all-changed refuse a tree that equals HEAD: an empty
    change set there is a mistake (nothing saved or staged), not a
    request to build HEAD."""
    if source == "staged":
        tree = gitutil.index_tree(repo, index_file)
    elif source == "all-changed":
        paths = gitutil.changed_paths(repo)
        tree = gitutil.build_tree(repo, index_file, [(p, None) for p in paths])
    else:
        tree = gitutil.build_tree(repo, index_file, specs)
    changes = gitutil.tree_changes(repo, "HEAD", tree)
    if not changes and source != "paths":
        why = "index matches HEAD" if source == "staged" else "no tracked changes"
        raise ToolError(
            f"--{source}: {why}, nothing to verify "
            "(name an unchanged PATH to build HEAD on purpose)"
        )
    return tree, changes


def format_taken(
    repo: Path, source: str, specs: list[tuple[str, Path | None]]
) -> list[str]:
    """Format in place the working-tree files the run takes (not under
    --staged: the index is what it builds, and PATH=FILE content is not
    a working-tree file), so the tree built is the formatted one; the
    result lines of what was rewritten or refused."""
    if source == "staged":
        return []
    if source == "paths":
        taken = [path for path, file in specs if file is None]
    else:
        taken = gitutil.changed_paths(repo)
    report = formatting.write(repo, [p for p in taken if (repo / p).is_file()])
    lines = report.lines("rewrote")
    for line in lines:
        print(f"dktools: format: {line}", file=sys.stderr, flush=True)
    return lines


def _source_label(source: str, specs: list[tuple[str, Path | None]]) -> str:
    if source == "staged":
        return "the index"
    if source == "all-changed":
        return "all changed paths"
    return f"{len(specs)} path(s)"


def _ctest_line(step: Step, result: steps.CtestResult) -> str:
    if step.code is None:
        return "not run"
    detail = result.summary or ("no tests matched" if result.no_tests else "no summary")
    if step.timed_out:
        return f"FAILED (timed out; {detail})"
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
        timeout=steps.QUERY_TIMEOUT,
    )
    commands_out = work / "test-commands.txt"
    steps.run_logged(
        "test-commands",
        steps.test_commands_command(bindir, commands_out),
        work / "test-commands.log",
        cwd=bindir,
        timeout=steps.QUERY_TIMEOUT,
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
    source = check_source(opts)
    specs = parse_specs(opts.specs) if source == "paths" else []
    config = toolchain.build_config(opts.compiler, opts.preset)
    size = procs.sizing(opts.jobs)

    # Every input check that can refuse runs before ROOT/verify/NAME is
    # recreated, so a refused run leaves the previous result in place.
    # The tree is written through a throwaway index outside it.
    head = gitutil.git_text(repo, "rev-parse", "--short", "HEAD")
    formatted = format_taken(repo, source, specs)
    with tempfile.TemporaryDirectory(prefix="dktools-verify-") as tmp:
        tree, changes = build_source_tree(repo, Path(tmp) / "index", source, specs)
    config.compiler.check()

    root = gitutil.scratch_root(opts.root, repo).resolve()
    work = buildstore.fresh_build(repo, root, "verify", name, head, "verify")
    src = work / "src"
    gitutil.export_tree(repo, tree, src)
    config.check_presets_file(src)
    fmt = formatting.check(src, formatting.tree_files(repo, tree))
    formatting.write_log(fmt, work / "format.log")
    bindir = config.binary_dir(src)

    steps.request_codemodel(bindir)
    configure = steps.run_logged(
        "configure",
        config.configure_command(src),
        work / "configure.log",
        cwd=src,
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    build = Step("build", work / "build.log")
    plan: TargetPlan | None = None
    if configure.ok:
        plan = _plan_targets(bindir, work, opts)
        build = steps.run_logged(
            "build",
            config.build_command(src, size.build, plan.targets, size.load),
            build.log,
            cwd=src,
            timeout=steps.BUILD_TIMEOUT,
        )
    warn = steps.count_warning_lines(build.log)
    notes = steps.count_linker_notes(build.log)

    ctest = Step("ctest", work / "ctest.log")
    serial = Step("ctest-serial", work / "ctest-serial.log")
    test_outputs: list[Path] = []
    if build.ok and opts.tests:
        ctest = steps.run_logged(
            "ctest",
            steps.ctest_command(
                bindir,
                size.test,
                opts.tests,
                exclude=[opts.serial or ""],
                load=size.load,
            ),
            ctest.log,
            cwd=bindir,
            timeout=steps.CTEST_TIMEOUT,
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
            timeout=steps.CTEST_TIMEOUT,
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

    failed = ctest_result.failed + serial_result.failed
    passed = (
        configure.ok
        and build.ok
        and warn == 0
        and (ctest.ok or not opts.tests)
        and (serial.ok or not opts.serial)
        and not sanitizer
        and alloc.ok
        and atomic.ok
        and output.ok
        and fmt.clean()
    )

    counts = f"{warn} warning lines, {notes} linker notes"
    if not configure.ok:
        build_line = f"FAILED (configure {configure.describe_failure()})"
    elif not build.ok:
        build_line = f"FAILED ({build.describe_failure()}), {counts}"
    else:
        build_line = f"ok, {counts}"
    lines = [
        f"verify {name}: {'PASS' if passed else 'FAIL'}",
        f"  tree      {tree} (HEAD {head} + {_source_label(source, specs)}, "
        f"{len(changes)} changed)",
        *(f"  changed   {status} {path}" for status, path in changes),
        f"  config    {config.label}",
        f"  targets   {plan.line() if plan else '-'}",
        f"  build     {build_line}",
        f"  ctest     {_ctest_line(ctest, ctest_result) if opts.tests else '-'}",
        f"  serial    {_ctest_line(serial, serial_result) if opts.serial else '-'}",
        f"  sanitizer {_sanitizer_line(sanitizer)}",
        f"  alloc     {'ok' if alloc.ok else 'FAILED'} ({steps.last_line(alloc.log)})",
        f"  atomic    {'ok' if atomic.ok else 'FAILED'} ({steps.last_line(atomic.log)})",
        f"  output    {'ok' if output.ok else 'FAILED'} ({steps.last_line(output.log)})",
        f"  format    {formatting.gate_line(fmt)}",
        *(f"  formatted {line}" for line in formatted),
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
        atomic.log,
        output.log,
        work / "format.log",
    ):
        if log.exists():
            lines.append(f"    {log}")
    for out in test_outputs:
        lines.append(f"    {out}")
    text = "\n".join(lines) + "\n"
    (work / "result.txt").write_text(text)
    sys.stdout.write(text)
    return 0 if passed else 1

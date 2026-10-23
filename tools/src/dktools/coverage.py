"""``dktools coverage``: source coverage of a revision, and the public API
no test calls.

Usage::

    dktools coverage [--rev REV] [--top N] [--skip REGEX] [--serial REGEX]
                     [--root DIR] [-j N]

Exports REV (default HEAD) with ``git archive``, builds the ``coverage``
preset (clang 22, Release, ``-fprofile-instr-generate
-fcoverage-mapping``, no sanitizers), runs every test with its own
profile file (the serial set one at a time, as validate does), merges
the profiles with ``llvm-profdata`` and reports with ``llvm-cov``:

- per-file line, function, branch and region coverage of the library
  sources (``deps/``, ``*Test.c``, ``*Bench.c`` and ``datakit-test.c``
  are left out; tests that live inside a module's own ``.c`` count with
  it), the totals, and the least-covered files;
- the public API: every function declared in a ``src/`` header (not
  ``*Internal.h`` or ``*Impl.h``; not the ``*Test`` entry points), split
  into called, never called (count 0), and not built (no counters: a
  declaration with no definition in the test binary, or a platform's).

Writes ROOT/coverage/<hash>/summary.md and summary.json and prints the
summary.  There is no percentage target: the output is a list of gaps.
Exits 0 when everything built and every test passed, 1 otherwise (the
report is still written when the build succeeded), 2 on an input error.
"""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import gitutil, procs, steps, toolchain, validate
from .gitutil import ToolError
from .steps import Step

#: Library sources only: vendored code, test files and benches are out.
IGNORE_FILES = r"(^|/)deps/|Test\.c$|Bench\.c$|(^|/)datakit-test\.c$"

#: Headers that are not public API: internal and implementation headers
#: and memshard's allocator internals (its public header is mimalloc.h).
PRIVATE_HEADER = re.compile(
    r"(Internal|Impl)\.h$|(^|/)memshard/(src|include/mimalloc)/"
)

#: Identifiers that look like a function name before '(' but are not:
#: return types of function-pointer declarations and keywords.
NOT_A_NAME = frozenset(
    "void int char short long unsigned signed float double bool size_t "
    "ssize_t const struct union enum return sizeof typedef static extern "
    "inline volatile restrict".split()
)


@dataclass
class CoverageOptions:
    rev: str = "HEAD"
    serial: str = validate.DEFAULT_SERIAL
    skip: str = validate.DEFAULT_SKIP
    top: int = 25
    root: Path | None = None
    jobs: int | None = None
    compiler: toolchain.Compiler | None = None


@dataclass
class FileCoverage:
    path: str
    lines: tuple[int, int]  # (covered, count)
    functions: tuple[int, int]
    branches: tuple[int, int]
    regions: tuple[int, int]

    @property
    def line_percent(self) -> float:
        covered, count = self.lines
        return 100.0 * covered / count if count else 100.0


@dataclass
class PublicApi:
    called: dict[str, list[str]] = field(default_factory=dict)  # header -> names
    never: dict[str, list[str]] = field(default_factory=dict)
    not_built: dict[str, list[str]] = field(default_factory=dict)
    # Header inline functions (any header) that no compiled copy ran
    unrun_inline: dict[str, list[str]] = field(default_factory=dict)

    def count(self, which: dict[str, list[str]]) -> int:
        return sum(len(v) for v in which.values())


# ---------------------------------------------------------------------------
# Parsing (pure: tested on canned text)
# ---------------------------------------------------------------------------


def _strip_comments_and_preprocessor(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    out: list[str] = []
    continuing = False
    for line in text.split("\n"):
        directive = continuing or line.lstrip().startswith("#")
        continuing = directive and line.rstrip().endswith("\\")
        if not directive:
            out.append(line)
    return "\n".join(out)


def declared_functions(header_text: str) -> list[str]:
    """Names of the functions a header declares with a prototype.

    File-scope statements only (anything inside braces is skipped, so
    inline bodies and struct members are not declarations); typedefs,
    static functions and macro invocations without a return type are
    not public functions.  ALL_CAPS names are template macros that
    expand to the real names (``DKS_NEW``), and ``deprecated``
    declarations exist only to stop callers using a libc function."""
    text = _strip_comments_and_preprocessor(header_text)
    statements: list[str] = []
    depth = 0
    parens = 0
    current: list[str] = []
    for ch in text:
        if ch == "{":
            if depth == 0:
                current = []  # a body or a struct: not a prototype
            depth += 1
        elif ch == "}":
            depth = max(depth - 1, 0)
        elif depth == 0:
            if ch == ";":
                statements.append("".join(current))
                current = []
                continue
            current.append(ch)
            parens += ch == "("
            parens -= ch == ")"
            # A macro invocation (ALL_CAPS(...)) needs no ';': end it when
            # its parentheses close so it cannot swallow the next statement
            if ch == ")" and parens == 0:
                so_far = "".join(current).strip()
                if re.fullmatch(r"[A-Z_][A-Z0-9_]*\s*\(.*\)", so_far, re.S):
                    statements.append(so_far)
                    current = []
    names: list[str] = []
    for statement in statements:
        flat = " ".join(statement.split())
        if not flat or flat.startswith(("typedef", "static", 'extern "C"')):
            continue
        match = re.match(r"^([^()]*?)\b([A-Za-z_]\w*)\s*\(", flat)
        if not match:
            continue
        prefix, name = match.group(1).strip(), match.group(2)
        if not prefix or name in NOT_A_NAME or not re.search(r"\w", prefix):
            continue
        if not re.search(r"[a-z]", name) or "deprecated" in flat:
            continue
        names.append(name)
    return names


#: A function a header defines: ``static inline`` or ``DK_INLINE_ALWAYS``
#: before its name, its parameter list, then its body.
INLINE_DEFINITION = re.compile(
    r"\b(?:static\s+inline|DK_INLINE_ALWAYS)\b[^;{}()]*?\b([A-Za-z_]\w*)\s*"
    r"\([^;{}]*\)\s*\{"
)


def inline_functions(header_text: str) -> list[str]:
    """Names of the functions a header defines inline (macro bodies are
    left out with the rest of the preprocessor lines)."""
    text = _strip_comments_and_preprocessor(header_text)
    names = {m.group(1) for m in INLINE_DEFINITION.finditer(text)}
    return sorted(names - NOT_A_NAME)


def unrun_inline(
    headers: dict[str, str], counts: dict[str, int]
) -> dict[str, list[str]]:
    """Header-defined functions that every compiled copy left unrun.

    A header's inline function gets one copy per file that includes it
    (``file.c:name`` in the profile), and the per-file line numbers count
    every unused copy as uncovered even when another copy runs.  Summed
    over its copies by name, a function counts as run when any copy ran;
    a name absent from the profile was never compiled and is left out."""
    totals: dict[str, int] = {}
    for key, value in counts.items():
        bare = key.rsplit(":", 1)[-1]
        totals[bare] = totals.get(bare, 0) + value
    unrun: dict[str, list[str]] = {}
    for header in sorted(headers):
        for name in inline_functions(headers[header]):
            if totals.get(name) == 0:
                unrun.setdefault(header, []).append(name)
    return unrun


def parse_profile_counts(text: str) -> dict[str, int]:
    """Function name -> entry count from ``llvm-profdata show
    --all-functions --counts``.  Static functions appear as
    ``file.c:name`` and are kept under that name."""
    counts: dict[str, int] = {}
    name: str | None = None
    for line in text.splitlines():
        header = re.match(r"^  (\S.*):$", line)
        if header:
            name = header.group(1)
            continue
        entry = re.match(r"^\s+Function count: (\d+)$", line)
        if entry and name is not None:
            counts[name] = counts.get(name, 0) + int(entry.group(1))
    return counts


def _pair(summary: dict, key: str) -> tuple[int, int]:
    item = summary.get(key, {})
    return int(item.get("covered", 0)), int(item.get("count", 0))


def parse_export_summary(
    text: str, source_root: Path
) -> tuple[list[FileCoverage], FileCoverage]:
    """Per-file rows and the totals row from ``llvm-cov export
    -summary-only`` JSON; paths are made relative to `source_root`."""
    data = json.loads(text)["data"][0]
    root = str(source_root.resolve()) + "/"
    files: list[FileCoverage] = []
    for item in data["files"]:
        path = item["filename"]
        if path.startswith(root):
            path = path[len(root) :]
        s = item["summary"]
        files.append(
            FileCoverage(
                path,
                _pair(s, "lines"),
                _pair(s, "functions"),
                _pair(s, "branches"),
                _pair(s, "regions"),
            )
        )
    t = data["totals"]
    total = FileCoverage(
        "TOTAL",
        _pair(t, "lines"),
        _pair(t, "functions"),
        _pair(t, "branches"),
        _pair(t, "regions"),
    )
    return files, total


def public_api(headers: dict[str, str], counts: dict[str, int]) -> PublicApi:
    """Split every public function into called / never called / not built."""
    api = PublicApi()
    for header in sorted(headers):
        for name in sorted(set(declared_functions(headers[header]))):
            if name.endswith("Test"):
                continue  # datakit-test entry points always run
            if name not in counts:
                api.not_built.setdefault(header, []).append(name)
            elif counts[name] == 0:
                api.never.setdefault(header, []).append(name)
            else:
                api.called.setdefault(header, []).append(name)
    return api


def collect_headers(src: Path, public_only: bool = True) -> dict[str, str]:
    """The headers under `src`/src (the public ones unless `public_only`
    is false), by repo-relative path."""
    headers: dict[str, str] = {}
    base = src / "src"
    for path in sorted(base.rglob("*.h")):
        rel = path.relative_to(src).as_posix()
        if (public_only and PRIVATE_HEADER.search(rel)) or "/deps/" in rel:
            continue
        headers[rel] = path.read_text(encoding="utf-8", errors="replace")
    return headers


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def _pct(pair: tuple[int, int]) -> str:
    covered, count = pair
    if count == 0:
        return "-"
    return f"{100.0 * covered / count:.1f}% ({covered}/{count})"


def render(
    commit: str,
    subject: str,
    tests: str,
    files: list[FileCoverage],
    total: FileCoverage,
    api: PublicApi,
    top: int,
) -> str:
    md = [f"# coverage {commit[:10]}", "", f"Revision `{commit}`: {subject}", ""]
    md += [f"- tests: {tests}", ""]
    md += ["## Totals (library sources)", ""]
    md += ["| lines | functions | branches | regions |", "| --- | --- | --- | --- |"]
    md.append(
        f"| {_pct(total.lines)} | {_pct(total.functions)} "
        f"| {_pct(total.branches)} | {_pct(total.regions)} |"
    )
    md += ["", f"## Least-covered files (by lines, {top} shown)", ""]
    md += [
        "| file | lines | functions | branches |",
        "| --- | --- | --- | --- |",
    ]
    ranked = sorted(
        (f for f in files if f.lines[1] > 0), key=lambda f: (f.line_percent, f.path)
    )
    for f in ranked[:top]:
        md.append(
            f"| {f.path} | {_pct(f.lines)} | {_pct(f.functions)} | {_pct(f.branches)} |"
        )
    # Where the uncovered lines are: a large file at 80% can hold more of
    # them than every small file at 0%.
    md += ["", f"## Most uncovered lines ({top} shown)", ""]
    md += [
        "| file | uncovered lines | lines | branches |",
        "| --- | --- | --- | --- |",
    ]
    by_missed = sorted(
        (f for f in files if f.lines[1] > f.lines[0]),
        key=lambda f: (-(f.lines[1] - f.lines[0]), f.path),
    )
    for f in by_missed[:top]:
        md.append(
            f"| {f.path} | {f.lines[1] - f.lines[0]} | {_pct(f.lines)} "
            f"| {_pct(f.branches)} |"
        )
    declared = api.count(api.called) + api.count(api.never) + api.count(api.not_built)
    md += ["", "## Public API", ""]
    md.append(
        f"- {declared} declared: {api.count(api.called)} called by the tests, "
        f"{api.count(api.never)} never called, "
        f"{api.count(api.not_built)} not built in the test binary"
    )
    for title, which in (
        ("Never called", api.never),
        ("Not built (no definition in the test binary)", api.not_built),
        (
            "Header inline functions no compiled copy ran (any header; a "
            "header's per-file line numbers also count the unused copies of "
            "functions that do run)",
            api.unrun_inline,
        ),
    ):
        md += ["", f"### {title}", ""]
        if not which:
            md.append("none")
        for header, names in which.items():
            md.append(f"- `{header}`: {', '.join(names)}")
    return "\n".join(md) + "\n"


def to_json(
    commit: str,
    tests_ok: bool,
    files: list[FileCoverage],
    total: FileCoverage,
    api: PublicApi,
) -> dict:
    def row(f: FileCoverage) -> dict:
        return {
            "path": f.path,
            "lines": list(f.lines),
            "functions": list(f.functions),
            "branches": list(f.branches),
            "regions": list(f.regions),
        }

    return {
        "commit": commit,
        "testsPassed": tests_ok,
        "totals": row(total),
        "files": [row(f) for f in files],
        "publicApi": {
            "called": api.called,
            "neverCalled": api.never,
            "notBuilt": api.not_built,
            "unrunInline": api.unrun_inline,
        },
    }


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------


def run(repo: Path, opts: CoverageOptions) -> int:
    """Build, test, report; return the exit code (ToolError: input error)."""
    if opts.jobs is not None and opts.jobs < 1:
        raise ToolError(f"-j must be at least 1 (got {opts.jobs})")
    if opts.top < 1:
        raise ToolError(f"--top must be at least 1 (got {opts.top})")
    compiler = opts.compiler or toolchain.resolve_compiler("clang22")
    compiler.check()
    llvm = Path(compiler.cc).parent
    profdata = str(llvm / "llvm-profdata")
    llvm_cov = str(llvm / "llvm-cov")
    for program in (profdata, llvm_cov):
        gitutil.require_program(program)
    jobs = opts.jobs or os.cpu_count() or 4

    commit = gitutil.resolve_commit(repo, opts.rev)
    subject = gitutil.git_text(repo, "log", "-1", "--format=%s", commit)
    root = gitutil.scratch_root(opts.root).resolve()
    work = gitutil.fresh_dir(root / "coverage" / commit[:10])
    src = work / "src"
    gitutil.export_tree(repo, commit, src)
    config = toolchain.build_config(compiler, "coverage")
    config.check_presets_file(src)
    bindir = config.binary_dir(src)
    logs = work / "logs"
    profiles = gitutil.fresh_dir(work / "profiles")

    configure = steps.run_logged(
        "coverage configure",
        config.configure_command(src),
        logs / "configure.log",
        cwd=src,
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    build = Step("build", logs / "build.log")
    if configure.ok:
        build = steps.run_logged(
            "coverage build",
            config.build_command(src, jobs, None),
            build.log,
            cwd=src,
            timeout=steps.BUILD_TIMEOUT,
        )
    if not build.ok:
        print(f"dktools coverage: build failed; see {build.log}", file=sys.stderr)
        return 1

    env = {"LLVM_PROFILE_FILE": str(profiles / "%p.profraw")}
    ctest = steps.run_logged(
        "coverage ctest",
        steps.ctest_command(bindir, jobs, None, exclude=[opts.serial, opts.skip]),
        logs / "ctest.log",
        cwd=bindir,
        env=env,
        timeout=steps.CTEST_TIMEOUT,
    )
    serial_here = validate.serial_for(opts.serial, [opts.skip])
    serial = Step("ctest-serial", logs / "ctest-serial.log", code=0)
    if serial_here:
        serial = steps.run_logged(
            "coverage ctest-serial",
            steps.ctest_command(bindir, 1, serial_here, exclude=[opts.skip]),
            serial.log,
            cwd=bindir,
            env=env,
            timeout=steps.CTEST_TIMEOUT,
        )
    results = [steps.parse_ctest(ctest.log), steps.parse_ctest(serial.log)]
    failed = [name for r in results for name in r.failed]
    tests_ok = ctest.ok and serial.ok
    tests = "all passed" if tests_ok else f"FAILED: {', '.join(failed) or 'see logs'}"

    raws = sorted(str(p) for p in profiles.glob("*.profraw"))
    if not raws:
        raise ToolError(f"no profiles were written under {profiles}")
    merged = work / "merged.profdata"
    merge = steps.run_logged(
        "coverage merge",
        [profdata, "merge", "-o", str(merged), *raws],
        logs / "merge.log",
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    binary = str(bindir / "src" / "datakit-test")
    show = steps.run_logged(
        "coverage functions",
        [profdata, "show", "--all-functions", "--counts", str(merged)],
        logs / "functions.txt",
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    export = steps.run_logged(
        "coverage export",
        [
            llvm_cov,
            "export",
            "-summary-only",
            binary,
            f"-instr-profile={merged}",
            f"-ignore-filename-regex={IGNORE_FILES}",
        ],
        logs / "export.json",
        timeout=steps.CONFIGURE_TIMEOUT,
    )
    if not (merge.ok and show.ok and export.ok):
        raise ToolError(f"llvm-profdata / llvm-cov failed; see {logs}")

    report(work, commit, subject, tests, tests_ok, opts.top)
    return 0 if tests_ok else 1


def report(
    work: Path, commit: str, subject: str, tests: str, tests_ok: bool, top: int
) -> None:
    """Write and print the summary of the run in `work` (its exported
    tree, profile listing and llvm-cov export)."""
    logs = work / "logs"
    src = work / "src"
    export_text = (logs / "export.json").read_text().split("\n", 1)[1]
    files, total = parse_export_summary(export_text, src)
    counts = parse_profile_counts((logs / "functions.txt").read_text())
    api = public_api(collect_headers(src), counts)
    api.unrun_inline = unrun_inline(collect_headers(src, public_only=False), counts)

    text = render(commit, subject, tests, files, total, api, top)
    (work / "summary.md").write_text(text)
    (work / "summary.json").write_text(
        json.dumps(to_json(commit, tests_ok, files, total, api), indent=1) + "\n"
    )
    sys.stdout.write(text)
    print(f"summary: {work / 'summary.md'}")


#: ``llvm-cov show`` line: ``  NN|  COUNT|source`` (COUNT empty for a
#: line with no code; 0 for code never run; 1.2k-style for large counts).
_SHOW_LINE_RE = re.compile(r"^\s*(\d+)\|\s*([0-9.]*[kMGTPE]?)\|(.*)$")


@dataclass
class UncoveredRange:
    first: int
    last: int
    text: str  # the first uncovered line's source, stripped


def parse_show_uncovered(text: str) -> list[UncoveredRange]:
    """Ranges of never-run code lines from ``llvm-cov show`` of one file.
    A range runs through lines without code between uncovered ones, and
    ends at the next line that ran."""
    ranges: list[UncoveredRange] = []
    current: UncoveredRange | None = None
    for raw in text.splitlines():
        m = _SHOW_LINE_RE.match(raw)
        if not m:
            continue
        line, count, source = int(m.group(1)), m.group(2), m.group(3)
        if count == "0":
            if current is None:
                current = UncoveredRange(line, line, source.strip())
                ranges.append(current)
            else:
                current.last = line
        elif count:
            current = None
    return ranges


@dataclass
class FunctionMiss:
    name: str
    lines: int
    missed: int


def parse_function_report(text: str) -> list[FunctionMiss]:
    """Functions with missed lines from ``llvm-cov report -show-functions``
    (columns: Name, Regions, Miss, Cover, Lines, Miss, Cover, ...), most
    missed first."""
    out: list[FunctionMiss] = []
    for raw in text.splitlines():
        parts = raw.split()
        if len(parts) < 7 or not parts[1].isdigit() or not parts[4].isdigit():
            continue
        if not parts[5].isdigit() or parts[0] in ("Name", "TOTAL"):
            continue
        lines, missed = int(parts[4]), int(parts[5])
        if missed:
            out.append(FunctionMiss(parts[0], lines, missed))
    out.sort(key=lambda f: (-f.missed, f.name))
    return out


def render_detail(
    path: str, functions: list[FunctionMiss], ranges: list[UncoveredRange]
) -> str:
    md = [f"## {path}", ""]
    if not functions and not ranges:
        md += ["every line ran", ""]
        return "\n".join(md) + "\n"
    md += ["### Functions with missed lines", ""]
    md += [f"- {f.name}: {f.missed} of {f.lines} lines missed" for f in functions]
    md += ["", "### Uncovered lines", ""]
    for r in ranges:
        span = f"{r.first}" if r.first == r.last else f"{r.first}-{r.last}"
        md.append(f"- {span}: `{r.text}`")
    return "\n".join(md) + "\n"


def _earlier_run(repo: Path, opts: CoverageOptions) -> tuple[str, Path]:
    commit = gitutil.resolve_commit(repo, opts.rev)
    work = gitutil.scratch_root(opts.root).resolve() / "coverage" / commit[:10]
    return commit, work


def detail(repo: Path, opts: CoverageOptions, paths: list[str]) -> int:
    """Per-file gaps (functions by missed lines, uncovered line ranges) from
    the earlier run for REV: no build, no tests."""
    commit, work = _earlier_run(repo, opts)
    src = work / "src"
    merged = work / "merged.profdata"
    compiler = opts.compiler or toolchain.resolve_compiler("clang22")
    llvm_cov = str(Path(compiler.cc).parent / "llvm-cov")
    binary = toolchain.build_config(compiler, "coverage").binary_dir(src) / "src"
    binary = binary / "datakit-test"
    for need in (merged, binary):
        if not need.exists():
            raise ToolError(
                f"no earlier coverage run for {commit[:10]}: {need} is missing"
            )

    def cov(command: list[str], source: Path) -> str:
        done = procs.run(
            [
                llvm_cov,
                *command,
                f"-instr-profile={merged}",
                str(binary),
                str(source),
            ],
            timeout=steps.CONFIGURE_TIMEOUT,
        )
        if done.returncode != 0:
            raise ToolError(
                f"llvm-cov {command[0]} failed: "
                f"{done.stderr.decode('utf-8', 'replace').strip()}"
            )
        return done.stdout.decode("utf-8", "replace")

    out = []
    for rel in paths:
        source = src / rel
        if not source.is_file():
            raise ToolError(f"{rel}: not a file of the exported tree")
        show = cov(["show", "-use-color=false"], source)
        report_text = cov(["report", "-show-functions", "-use-color=false"], source)
        out.append(
            render_detail(
                rel,
                parse_function_report(report_text),
                parse_show_uncovered(show),
            )
        )
    sys.stdout.write("\n".join(out))
    return 0


def rereport(repo: Path, opts: CoverageOptions) -> int:
    """Render the summary again from an earlier run's results for REV,
    with no build and no tests (the report's own changes, cheaply)."""
    commit = gitutil.resolve_commit(repo, opts.rev)
    subject = gitutil.git_text(repo, "log", "-1", "--format=%s", commit)
    work = gitutil.scratch_root(opts.root).resolve() / "coverage" / commit[:10]
    logs = work / "logs"
    for need in (work / "src", logs / "export.json", logs / "functions.txt"):
        if not need.exists():
            raise ToolError(
                f"no earlier coverage run for {commit[:10]}: {need} is missing"
            )
    results = [
        steps.parse_ctest(logs / name)
        for name in ("ctest.log", "ctest-serial.log")
        if (logs / name).exists()
    ]
    failed = [name for r in results for name in r.failed]
    tests_ok = not failed
    tests = "all passed (earlier run)" if tests_ok else f"FAILED: {', '.join(failed)}"
    report(work, commit, subject, tests, tests_ok, top=opts.top)
    return 0 if tests_ok else 1

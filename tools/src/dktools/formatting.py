"""``dktools format``: the project's formatters, run on a set of files.

Each kind of file has one formatter:

- C sources and headers (``.c``, ``.h``): clang-format with the
  repository's ``.clang-format``, the clang-format in the LLVM bin
  directory clang 22 comes from (``$DKTOOLS_LLVM_DIR``);
- Markdown (``.md``): prettier (which reads ``.prettierignore`` from the
  directory it runs in);
- Python (``.py``): ``ruff check --fix`` with ``RUFF_RULES``, then
  ``ruff format``, the ruff locked in ``tools/uv.lock`` (dktools' own
  environment, never a global install).

``write`` rewrites files in place and reports the files it changed;
``check`` changes nothing and reports the files a write would change.
Either way a lint finding ruff cannot fix is a problem, and so is a
formatter failure (a file it cannot parse, a missing program); a run
with problems fails.  Every tool runs in the tree's root, so each finds
the configuration of the tree it formats.  ``deps/`` is vendored, and
formatted by the projects it comes from: it is never touched.
"""

from __future__ import annotations

import hashlib
import os
import re
import sys
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from . import gitutil, procs, toolchain

#: Repository-relative path prefixes no formatter touches.
EXCLUDED = ("deps/",)

#: The ruff lint rules every Python file passes.
RUFF_RULES = (
    "--select",
    "SIM,PTH,I,UP,FURB,PGH",
    "--extend-select",
    "F401",
    "--ignore",
    "SIM102,SIM110",
)

#: Files per formatter invocation (keeps argument lists short).
CHUNK = 200

#: Wall-clock limit of one formatter invocation, in seconds.
TIMEOUT = 10 * 60


@dataclass
class Report:
    """What a format run did, per formatter."""

    files: int = 0
    #: write: files rewritten; check: files a write would rewrite.
    changed: dict[str, list[str]] = field(default_factory=dict)
    #: Files a formatter refused or ruff could not fix, and why.
    problems: dict[str, list[str]] = field(default_factory=dict)
    #: Every command and its output, for a log.
    log: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.problems

    def clean(self) -> bool:
        """True when nothing changed (check: the files are formatted)."""
        return not self.changed and not self.problems

    def lines(self, changed_verb: str) -> list[str]:
        """One line per formatter with changed or problem files."""
        out = []
        for tool, files in self.changed.items():
            out.append(f"{tool}: {changed_verb} {len(files)}: {' '.join(files)}")
        for tool, files in self.problems.items():
            out.append(f"{tool}: problems in {len(files)}: {' '.join(files)}")
        return out


# -- which files ------------------------------------------------------


def surface(paths: Iterable[str]) -> dict[str, list[str]]:
    """`paths` grouped by the formatter that owns them (sorted, unique);
    paths no formatter owns and paths under ``EXCLUDED`` are dropped."""
    groups: dict[str, set[str]] = {}
    for path in paths:
        if path.startswith(EXCLUDED):
            continue
        for formatter in FORMATTERS:
            if path.endswith(formatter.suffixes):
                groups.setdefault(formatter.name, set()).add(path)
                break
    return {name: sorted(files) for name, files in groups.items()}


def _untracked(repo: Path) -> list[str]:
    return gitutil.split_z(
        gitutil.git(repo, "ls-files", "-z", "--others", "--exclude-standard")
    )


def changed_files(repo: Path) -> list[str]:
    """Files that differ from HEAD (modified or added) plus untracked
    files git does not ignore; deleted files are left out."""
    paths = gitutil.changed_paths(repo) + _untracked(repo)
    return [p for p in paths if (repo / p).is_file()]


def all_files(repo: Path) -> list[str]:
    """Every tracked file plus untracked files git does not ignore."""
    paths = gitutil.split_z(gitutil.git(repo, "ls-files", "-z")) + _untracked(repo)
    return [p for p in paths if (repo / p).is_file()]


def tree_files(repo: Path, treeish: str) -> list[str]:
    """Every file of a git tree (what an exported tree holds)."""
    return list(gitutil.ls_tree(repo, treeish))


# -- running the tools -------------------------------------------------


def _chunks(files: Sequence[str]) -> Iterable[list[str]]:
    for i in range(0, len(files), CHUNK):
        yield list(files[i : i + CHUNK])


def _run(
    root: Path, cmd: list[str], files: list[str], report: Report
) -> tuple[int, list[str]]:
    """Run `cmd` on `files` in `root`; (exit code, output lines), logged."""
    report.log.append("$ " + " ".join(cmd) + f" <{len(files)} files>")
    try:
        done = procs.run([*cmd, *files], cwd=root, timeout=TIMEOUT)
    except (procs.ToolError, procs.TimedOut) as exc:
        report.log.append(str(exc))
        return 127, [str(exc)]
    text = (done.stdout + done.stderr).decode("utf-8", "replace")
    lines = text.splitlines()
    report.log.extend(lines)
    return done.returncode, lines


def _digest(path: Path) -> str:
    try:
        return hashlib.blake2b(path.read_bytes(), digest_size=16).hexdigest()
    except OSError:
        return ""


def _files_named(lines: Iterable[str], files: Sequence[str], pattern) -> list[str]:
    """The files of `files` that `pattern` (group "file") names in `lines`."""
    wanted = set(files)
    found = set()
    for line in lines:
        m = pattern.match(line.strip())
        if m:
            groups = m.groupdict()
            name = os.path.normpath(groups.get("file") or groups.get("at") or "")
            if name in wanted:
                found.add(name)
    return sorted(found)


@dataclass(frozen=True)
class Formatter:
    """One formatter: its file suffixes, and how to write and check.

    `write` and `check` each take (root, files, report) and return the
    files with problems; `check` also returns the files it would change.
    """

    name: str
    suffixes: tuple[str, ...]
    write: Callable[[Path, list[str], Report], list[str]]
    check: Callable[[Path, list[str], Report], tuple[list[str], list[str]]]


def _clang_format() -> str:
    bindir = os.environ.get(toolchain.ENV_LLVM_DIR) or toolchain.DEFAULT_LLVM_DIR
    return str(Path(bindir) / "clang-format")


_CLANG_VIOLATION = re.compile(r"^(?P<file>[^:]+):\d+:\d+: (?:error|warning): ")


def _clang_write(root: Path, files: list[str], report: Report) -> list[str]:
    problems: list[str] = []
    for chunk in _chunks(files):
        cmd = [_clang_format(), "--style=file", "-i"]
        code, lines = _run(root, cmd, chunk, report)
        if code:
            problems += _files_named(lines, chunk, _CLANG_VIOLATION) or chunk
    return problems


def _clang_check(root: Path, files: list[str], report: Report):
    changed: list[str] = []
    problems: list[str] = []
    for chunk in _chunks(files):
        cmd = [
            _clang_format(),
            "--style=file",
            "--dry-run",
            "-Werror",
            "--ferror-limit=1",
        ]
        code, lines = _run(root, cmd, chunk, report)
        named = _files_named(
            (line for line in lines if "[-Wclang-format-violations]" in line),
            chunk,
            _CLANG_VIOLATION,
        )
        changed += named
        if code and not named:
            problems += chunk
    return changed, problems


#: prettier reads .gitignore as an ignore file by default, which would
#: skip every internal (gitignored) note named explicitly; only the
#: prettier-specific ignore file applies (absent in exported trees: then
#: nothing is ignored).
PRETTIER_IGNORE = ("--ignore-path", ".prettierignore")


def _prettier() -> str:
    return gitutil.require_program("prettier")


def _prettier_write(root: Path, files: list[str], report: Report) -> list[str]:
    problems: list[str] = []
    for chunk in _chunks(files):
        try:
            cmd = [_prettier(), *PRETTIER_IGNORE, "--write", "--log-level", "warn"]
        except procs.ToolError as exc:
            report.log.append(str(exc))
            return list(files)
        code, _ = _run(root, cmd, chunk, report)
        if code:
            problems += chunk
    return problems


def _prettier_check(root: Path, files: list[str], report: Report):
    changed: list[str] = []
    problems: list[str] = []
    for chunk in _chunks(files):
        try:
            cmd = [_prettier(), *PRETTIER_IGNORE, "--list-different"]
        except procs.ToolError as exc:
            report.log.append(str(exc))
            return [], list(files)
        code, lines = _run(root, cmd, chunk, report)
        wanted = set(chunk)
        changed += sorted(os.path.normpath(x) for x in lines if x.strip() in wanted)
        if code not in (0, 1):  # 1: some files differ; 2: an error
            problems += chunk
    return changed, problems


def _ruff(*args: str) -> list[str]:
    """ruff from dktools' own environment (a locked dependency in
    tools/uv.lock), never a global install."""
    return [sys.executable, "-m", "ruff", *args]


_RUFF_FINDING = re.compile(r"^(?P<file>[^:]+):\d+:\d+: ")
#: ``ruff format --check`` names a file it would change either way.
_RUFF_REFORMAT = re.compile(
    r"^(?:Would reformat: (?P<file>.+)|--> (?P<at>[^:]+):\d+:\d+)$"
)


def _ruff_write(root: Path, files: list[str], report: Report) -> list[str]:
    try:
        lint = _ruff("check", *RUFF_RULES, "--fix", "--output-format", "concise")
        fmt = _ruff("format")
    except procs.ToolError as exc:
        report.log.append(str(exc))
        return list(files)
    problems: list[str] = []
    for chunk in _chunks(files):
        code, lines = _run(root, lint, chunk, report)
        if code:
            problems += _files_named(lines, chunk, _RUFF_FINDING) or chunk
        code, _ = _run(root, fmt, chunk, report)
        if code:
            problems += chunk
    return sorted(set(problems))


def _ruff_check(root: Path, files: list[str], report: Report):
    try:
        lint = _ruff("check", *RUFF_RULES, "--output-format", "concise")
        fmt = _ruff("format", "--check")
    except procs.ToolError as exc:
        report.log.append(str(exc))
        return [], list(files)
    changed: list[str] = []
    problems: list[str] = []
    for chunk in _chunks(files):
        code, lines = _run(root, lint, chunk, report)
        if code:
            # "[*]" marks a finding --fix fixes: a write changes the file
            fixable = [line for line in lines if " [*] " in line]
            unfixable = [line for line in lines if " [*] " not in line]
            changed += _files_named(fixable, chunk, _RUFF_FINDING)
            named = _files_named(unfixable, chunk, _RUFF_FINDING)
            problems += named if named or fixable else chunk
        code, lines = _run(root, fmt, chunk, report)
        named = _files_named(lines, chunk, _RUFF_REFORMAT)
        changed += named
        if code and not named:
            problems += chunk
    return sorted(set(changed)), sorted(set(problems))


FORMATTERS = (
    Formatter("clang-format", (".c", ".h"), _clang_write, _clang_check),
    Formatter("prettier", (".md",), _prettier_write, _prettier_check),
    Formatter("ruff", (".py",), _ruff_write, _ruff_check),
)


def write(root: Path, paths: Iterable[str]) -> Report:
    """Format `paths` (relative to `root`) in place."""
    report = Report()
    groups = surface(paths)
    for formatter in FORMATTERS:
        files = groups.get(formatter.name, [])
        if not files:
            continue
        report.files += len(files)
        before = {f: _digest(root / f) for f in files}
        problems = formatter.write(root, files, report)
        rewritten = [f for f in files if _digest(root / f) != before[f]]
        if rewritten:
            report.changed[formatter.name] = rewritten
        if problems:
            report.problems[formatter.name] = sorted(set(problems))
    return report


def check(root: Path, paths: Iterable[str]) -> Report:
    """Report the files of `paths` a write would change; change nothing."""
    report = Report()
    groups = surface(paths)
    for formatter in FORMATTERS:
        files = groups.get(formatter.name, [])
        if not files:
            continue
        report.files += len(files)
        changed, problems = formatter.check(root, files, report)
        if changed:
            report.changed[formatter.name] = sorted(set(changed))
        if problems:
            report.problems[formatter.name] = sorted(set(problems))
    return report


def gate_line(report: Report) -> str:
    """The verify/validate result cell of a check run."""
    if report.clean():
        return f"ok ({report.files} files)"
    bad = sorted(
        {
            f
            for files in (*report.changed.values(), *report.problems.values())
            for f in files
        }
    )
    shown = " ".join(bad[:8]) + (f" ... +{len(bad) - 8}" if len(bad) > 8 else "")
    return f"FAILED ({len(bad)} files; dktools format write fixes formatting: {shown})"


def run(repo: Path, action: str, paths: Sequence[str], every: bool) -> int:
    """``dktools format write|check``: on `paths`, every file (`every`)
    or by default the files changed from HEAD plus untracked ones."""
    if paths and every:
        raise procs.ToolError("--all takes no PATH arguments")
    if paths:
        chosen = [gitutil.repo_path(p) for p in paths]
        absent = [p for p in chosen if not (repo / p).is_file()]
        if absent:
            raise procs.ToolError(f"not a file: {' '.join(absent)}")
    else:
        chosen = all_files(repo) if every else changed_files(repo)
    report = write(repo, chosen) if action == "write" else check(repo, chosen)
    verb = "rewrote" if action == "write" else "would change"
    for line in report.lines(verb):
        print(line)
    if not report.ok:
        for line in report.log:
            print(line)
    if action == "check":
        print(f"format check: {gate_line(report)}")
        return 0 if report.clean() else 1
    rewritten = sum(len(f) for f in report.changed.values())
    state = "ok" if report.ok else "FAILED"
    print(f"format write: {state} ({report.files} files, {rewritten} rewritten)")
    return 0 if report.ok else 1


def write_log(report: Report, log: Path) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("\n".join([*report.log, *report.lines("would change"), ""]))

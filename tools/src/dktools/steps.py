"""Build, test and log-scanning steps shared by verify and validate.

Each step runs one program with an argument list, sends its combined
output to a log file, and returns a ``Step`` with the exit code; the
scanners read those logs back (compiler warnings, ctest results,
sanitizer reports) so every result line can name the log behind it.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from .gitutil import ToolError


@dataclass
class Step:
    """One logged program run."""

    name: str
    log: Path
    code: int | None = None  # None: not run

    @property
    def ok(self) -> bool:
        return self.code == 0


def run_logged(
    name: str,
    cmd: Sequence[str],
    log: Path,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
) -> Step:
    """Run `cmd`, stdout and stderr both to `log`; never raises on exit."""
    log.parent.mkdir(parents=True, exist_ok=True)
    print(f"dktools: {name}: {' '.join(cmd)}", file=sys.stderr, flush=True)
    full_env = None
    if env is not None:
        full_env = dict(os.environ)
        full_env.update(env)
    with open(log, "wb") as out:
        out.write(("$ " + " ".join(cmd) + "\n").encode())
        out.flush()
        try:
            proc = subprocess.run(
                list(cmd),
                cwd=cwd,
                stdout=out,
                stderr=subprocess.STDOUT,
                env=full_env,
                check=False,
            )
        except FileNotFoundError as exc:
            raise ToolError(f"{cmd[0]}: not found on PATH") from exc
    return Step(name, log, proc.returncode)


def read_lines(path: Path) -> list[str]:
    """`path`'s lines, decoded leniently; [] when the file is absent."""
    try:
        return path.read_bytes().decode("utf-8", "replace").splitlines()
    except FileNotFoundError:
        return []


# -- compiler warnings ------------------------------------------------

_WARNING = re.compile(
    r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:(?P<col>\d+):)?\s*warning:\s*(?P<msg>.*)$"
)


@dataclass(frozen=True)
class Warning_:
    """One distinct compiler warning (file None: no source location)."""

    file: str | None
    line: int
    text: str


#: The note macOS ``dsymutil`` prints when asked to collect the debug
#: info of an executable that was linked without any (vendored builds
#: run it after linking each test executable, also in Release builds).
#: It is a linker-stage note, not a diagnostic about the code: verify
#: and validate count it apart and never fail on it.  Only this exact
#: shape matches; every other ``warning:`` line, ``ld: warning:``
#: included, still counts as a warning.
_LINKER_NOTE = re.compile(
    r"^warning: no debug symbols in executable \(-arch [^)\s]+\)$"
)


def is_linker_note(line: str) -> bool:
    """True for the macOS "no debug symbols in executable" note."""
    return _LINKER_NOTE.match(line.strip()) is not None


def _is_warning(line: str) -> bool:
    return "warning:" in line and not is_linker_note(line)


def count_warning_lines(log: Path) -> int:
    """Lines containing ``warning:`` (compiler and linker warnings),
    except the macOS linker notes ``count_linker_notes`` counts."""
    return sum(1 for line in read_lines(log) if _is_warning(line))


def count_linker_notes(log: Path) -> int:
    """macOS "no debug symbols in executable" note lines in a build log."""
    return sum(1 for line in read_lines(log) if is_linker_note(line))


def warnings(log: Path, tree: Path) -> list[Warning_]:
    """Distinct warnings in a build log, paths relative to `tree`.

    A source compiled into both the static and shared library warns
    twice with the same location and text; those count once.  Paths
    are normalized first: a header reached as ``src/../deps/x.h`` is
    ``deps/x.h``.  Linker notes are not warnings and are left out."""
    root = os.path.normpath(tree.resolve()) + os.sep
    root_unresolved = os.path.normpath(tree) + os.sep
    seen: dict[tuple[str | None, int, str], Warning_] = {}
    for line in read_lines(log):
        if not _is_warning(line):
            continue
        m = _WARNING.match(line)
        if m is None:
            w = Warning_(None, 0, line.strip())
        else:
            path = os.path.normpath(m["file"])
            for prefix in (root, root_unresolved):
                if path.startswith(prefix):
                    path = path[len(prefix) :]
                    break
            w = Warning_(path, int(m["line"]), m["msg"].strip())
        seen.setdefault((w.file, w.line, w.text), w)
    return list(seen.values())


def group_warnings(found: Iterable[Warning_]) -> dict[str, dict[str, int]]:
    """{"src": {file: n}, "deps": {...}, "other": {...}} by tree area."""
    groups: dict[str, dict[str, int]] = {"src": {}, "deps": {}, "other": {}}
    for w in found:
        if w.file is None:
            area, key = "other", "(no source location)"
        elif w.file.startswith("src/"):
            area, key = "src", w.file
        elif w.file.startswith("deps/"):
            area, key = "deps", w.file
        else:
            area, key = "other", w.file
        groups[area][key] = groups[area].get(key, 0) + 1
    return groups


# -- ctest ------------------------------------------------------------

#: "100% tests passed out of 3" (CMake 4) or "..., 0 tests failed out of 3".
_CTEST_SUMMARY = re.compile(r"\d+% tests passed(?:, \d+ tests? failed)? out of \d+")
_CTEST_FAILED = re.compile(r"^\s*\d+\s+-\s+(?P<name>\S+)\s+\((?P<why>[^)]*)\)")


@dataclass
class CtestResult:
    """What a ctest run printed."""

    summary: str = ""
    failed: list[str] = field(default_factory=list)
    not_run: list[str] = field(default_factory=list)
    no_tests: bool = False


def anchored(regex: str) -> str:
    """A ctest -R/-E regex matching whole test names only."""
    return f"^({regex})$"


def ctest_command(
    binary_dir: Path,
    jobs: int,
    include: str | None = None,
    exclude: Sequence[str] = (),
) -> list[str]:
    """ctest argv; `include` and each `exclude` regex match whole names.

    No matching test is an error, so a mistyped regex cannot pass."""
    cmd = [
        "ctest",
        "--test-dir",
        str(binary_dir),
        "-j",
        str(jobs),
        "--output-on-failure",
        "--no-tests=error",
    ]
    if include:
        cmd += ["-R", anchored(include)]
    excluded = [e for e in exclude if e]
    if excluded:
        cmd += ["-E", anchored("|".join(excluded))]
    return cmd


def parse_ctest(log: Path) -> CtestResult:
    """Summary line, failed and not-run test names from ctest output."""
    result = CtestResult()
    section: list[str] | None = None
    for line in read_lines(log):
        m = _CTEST_SUMMARY.search(line)
        if m:
            result.summary = m.group(0)
            section = None
            continue
        if "No tests were found" in line:
            result.no_tests = True
        if line.startswith("The following tests FAILED"):
            section = result.failed
            continue
        if line.startswith("The following tests did not run"):
            section = result.not_run
            continue
        if section is not None:
            fm = _CTEST_FAILED.match(line)
            if fm:
                section.append(fm["name"])
            elif line.strip():
                section = None
    return result


def keep_test_log(binary_dir: Path, dest: Path) -> Path | None:
    """Copy ctest's full per-test output log (LastTest.log) to `dest`."""
    src = binary_dir / "Testing" / "Temporary" / "LastTest.log"
    if not src.exists():
        return None
    shutil.copyfile(src, dest)
    return dest


# -- sanitizers -------------------------------------------------------

#: Report headers: UBSan prints "runtime error:", ASan and LSan print
#: "ERROR: AddressSanitizer" / "ERROR: LeakSanitizer".
_SANITIZER = re.compile(
    r"runtime error:|ERROR: (?:AddressSanitizer|LeakSanitizer|ThreadSanitizer)"
)


@dataclass(frozen=True)
class SanitizerHit:
    log: Path
    line: int
    text: str


def sanitizer_reports(logs: Iterable[Path]) -> list[SanitizerHit]:
    """Every sanitizer report line in the given logs."""
    hits: list[SanitizerHit] = []
    for log in logs:
        for n, line in enumerate(read_lines(log), start=1):
            if _SANITIZER.search(line):
                hits.append(SanitizerHit(log, n, line.strip()))
    return hits


# -- allocation audit -------------------------------------------------


def allocaudit_command(uv: str, tree: Path) -> list[str]:
    """The allocOwnership gate, run with the tree's own dktools.

    The tree's copy matters: its CONSTRUCTORS registry must match the
    sources being audited."""
    return [
        uv,
        "run",
        "--frozen",
        "--project",
        str(tree / "tools"),
        "dktools",
        "--repo",
        str(tree),
        "allocaudit",
        "check",
        "--constructors",
    ]


def last_line(log: Path) -> str:
    lines = [line for line in read_lines(log) if line.strip()]
    return lines[-1].strip() if lines else ""

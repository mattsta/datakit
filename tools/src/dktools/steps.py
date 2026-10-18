"""Build, test and log-scanning steps shared by verify and validate.

Each step runs one program with an argument list, sends its combined
output to a log file, and returns a ``Step`` with the exit code; the
scanners read those logs back (compiler warnings, ctest results,
sanitizer reports) so every result line can name the log behind it.
"""

from __future__ import annotations

import json
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
#: ctest's line for a test whose executable does not exist, printed
#: right before that test's "***Not Run" result line.
_CTEST_NO_EXE = re.compile(r"^Unable to find executable:\s*(?P<exe>.+?)\s*$")
_CTEST_NOT_RUN = re.compile(
    r"^\s*\d+/\d+\s+Test\s+#\d+:\s+(?P<name>\S+)\s+\.*\s*\*\*\*Not Run\b"
)


@dataclass
class CtestResult:
    """What a ctest run printed.

    `missing` maps each test that could not start because its
    executable was never built to that executable; those tests are
    left out of `failed`, which holds the tests that ran and failed
    (or did not run for another reason)."""

    summary: str = ""
    failed: list[str] = field(default_factory=list)
    not_run: list[str] = field(default_factory=list)
    missing: dict[str, str] = field(default_factory=dict)
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
    """Summary line, failed, not-run and missing-executable test names
    from ctest output."""
    result = CtestResult()
    section: list[str] | None = None
    no_exe: str | None = None
    for line in read_lines(log):
        exe = _CTEST_NO_EXE.match(line)
        if exe:
            no_exe = exe["exe"]
            continue
        not_run = _CTEST_NOT_RUN.match(line)
        if not_run:
            if no_exe is not None:
                result.missing[not_run["name"]] = no_exe
            no_exe = None
            continue
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
    result.failed = [name for name in result.failed if name not in result.missing]
    return result


def keep_test_log(binary_dir: Path, dest: Path) -> Path | None:
    """Copy ctest's full per-test output log (LastTest.log) to `dest`."""
    src = binary_dir / "Testing" / "Temporary" / "LastTest.log"
    if not src.exists():
        return None
    shutil.copyfile(src, dest)
    return dest


# -- the targets selected tests need ----------------------------------
#
# A targeted build (the four preset targets) leaves out executables
# such as deps/varint's test programs, and ctest then reports their
# tests "Not Run".  CMake's own records name the targets a test needs:
#
# - ``ctest --show-only=json-v1 -R REGEX`` names the selected tests,
#   with ctest's own regex rules.  It leaves out the command of a test
#   whose executable is not built yet, so it cannot give the commands.
# - ``ctestCommands.cmake`` (``cmake -P``) evaluates the build's
#   CTestTestfile.cmake files and records every test's command line,
#   built or not.
# - The File API codemodel, requested before configure, gives every
#   target's artifacts.
#
# A test needs each target whose artifact its command line names.

#: The File API query file asking configure for the codemodel.
_CODEMODEL_QUERY = Path(".cmake", "api", "v1", "query", "codemodel-v2")


def request_codemodel(binary_dir: Path) -> None:
    """Ask the next configure of `binary_dir` to write the codemodel."""
    query = binary_dir / _CODEMODEL_QUERY
    query.parent.mkdir(parents=True, exist_ok=True)
    query.touch()


def _read_json(path: Path) -> dict | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def _real(binary_dir: Path, path: str) -> str:
    return os.path.realpath(binary_dir / path)


def target_artifacts(binary_dir: Path) -> dict[str, str]:
    """Artifact path (resolved) -> target name, from the codemodel
    configure wrote; {} when there is none."""
    reply = binary_dir / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        return {}
    index = _read_json(indexes[-1]) or {}
    entry = index.get("reply", {}).get("codemodel-v2", {})
    if not isinstance(entry, dict) or "jsonFile" not in entry:
        return {}
    codemodel = _read_json(reply / entry["jsonFile"]) or {}
    artifacts: dict[str, str] = {}
    for configuration in codemodel.get("configurations", []):
        for ref in configuration.get("targets", []):
            target = _read_json(reply / ref.get("jsonFile", "")) or {}
            name = target.get("name") or ref.get("name")
            for artifact in target.get("artifacts", []):
                path = artifact.get("path")
                if name and path:
                    artifacts[_real(binary_dir, path)] = name
    return artifacts


def list_tests_command(binary_dir: Path, include: str) -> list[str]:
    """ctest argv printing the tests `include` matches as JSON."""
    return [
        "ctest",
        "--test-dir",
        str(binary_dir),
        "--show-only=json-v1",
        "-R",
        anchored(include),
    ]


def parse_test_names(log: Path) -> list[str]:
    """The test names in a logged ``--show-only=json-v1`` run (the
    log's leading ``$ argv`` line is skipped); [] when the log holds
    no test list."""
    lines = read_lines(log)
    if lines and lines[0].startswith("$ "):
        lines = lines[1:]
    try:
        data = json.loads("\n".join(lines))
    except ValueError:
        return []
    tests = data.get("tests", []) if isinstance(data, dict) else []
    return [t["name"] for t in tests if isinstance(t.get("name"), str)]


#: The cmake -P script that records every test's command line.
CTEST_COMMANDS_SCRIPT = Path(__file__).with_name("ctestCommands.cmake")


def test_commands_command(binary_dir: Path, out: Path) -> list[str]:
    """cmake argv writing every test's command line to `out`."""
    return [
        "cmake",
        f"-DDIR={binary_dir}",
        f"-DOUT={out}",
        "-P",
        str(CTEST_COMMANDS_SCRIPT),
    ]


def parse_test_commands(out: Path) -> dict[str, list[str]]:
    """Test name -> command line from ctestCommands.cmake's output
    (records end with ASCII 30, fields split by ASCII 31); {} when the
    file is absent."""
    try:
        text = out.read_bytes().decode("utf-8", "surrogateescape")
    except FileNotFoundError:
        return {}
    commands: dict[str, list[str]] = {}
    for record in text.split("\x1e"):
        if record:
            name, *command = record.split("\x1f")
            commands[name] = command
    return commands


def targets_for_tests(
    commands: Mapping[str, Sequence[str]], artifacts: Mapping[str, str]
) -> dict[str, list[str]]:
    """Test name -> the targets whose artifacts its command line runs
    or names by absolute path (tests needing no built target, such as
    a ``sh`` gate script, are left out)."""
    needed: dict[str, list[str]] = {}
    for test, command in commands.items():
        targets = [
            artifacts[real]
            for real in (os.path.realpath(arg) for arg in command if os.path.isabs(arg))
            if real in artifacts
        ]
        if targets:
            needed[test] = list(dict.fromkeys(targets))
    return needed


# -- sanitizers -------------------------------------------------------

#: Report headers: UBSan prints "runtime error:", ASan and LSan print
#: "ERROR: AddressSanitizer" / "ERROR: LeakSanitizer".
_SANITIZER = re.compile(
    r"runtime error:|ERROR: (?:AddressSanitizer|LeakSanitizer|ThreadSanitizer)"
)


#: ctest's per-test output log (LastTest.log) opens each test's section
#: with "N/M Testing: NAME"; with -j the sections stay whole, each
#: written when its test finishes.
_TEST_SECTION = re.compile(r"^\d+/\d+ Testing: (?P<name>\S+)\s*$")


@dataclass(frozen=True)
class SanitizerHit:
    """One sanitizer report: its first line, where it is, which test."""

    log: Path
    line: int
    text: str
    test: str | None = None  # None: outside any test's section

    def describe(self) -> str:
        """``TEST: FIRST LINE (LOG:LINE)``, the first line unshortened."""
        return f"{self.test or '(unknown test)'}: {self.text} ({self.log.name}:{self.line})"


def sanitizer_reports(logs: Iterable[Path]) -> list[SanitizerHit]:
    """The first line of every sanitizer report in the given logs, with
    the test whose LastTest.log section it sits in."""
    hits: list[SanitizerHit] = []
    for log in logs:
        test: str | None = None
        for n, line in enumerate(read_lines(log), start=1):
            section = _TEST_SECTION.match(line)
            if section:
                test = section["name"]
            elif _SANITIZER.search(line):
                hits.append(SanitizerHit(log, n, line.strip(), test))
    return hits


def tests_with_reports(hits: Iterable[SanitizerHit]) -> list[str]:
    """The distinct test names behind `hits`, in first-report order."""
    return list(dict.fromkeys(h.test or "(unknown test)" for h in hits))


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

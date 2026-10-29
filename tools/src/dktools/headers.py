"""``dktools headers``: every header compiles on its own.

``dktools headers [--build DIR | --preset P] [-j N] [FILE ...]``

Each tracked ``src/**/*.h`` (the vendored trees aside) is compiled alone:
a one-line unit that includes it, with the compile command the build
uses for ``src/datakit.c`` (the library's flags, include paths and
defines, test builds' included), ``-fsyntax-only`` and no ``-Werror``, so
only an error fails.  A header that relies on what its includer brought
in first (a type, a macro, another header) fails here instead of in the
next file that includes it in another order.

Exceptions are ``PATH REASON`` lines in tools/headers-allow.txt (macro
templates, which compile only where an includer has defined their type
macros); the reason is required, and a line naming a header that now
compiles on its own, or one that is not a checked header, fails too, so
the list stays true.

Every handle in the public headers is also checked for its ``@handle``
tag (``dktools handles``: whether a call can move it), so the gate fails
on an untagged handle or one whose calls contradict its tag; and every
struct body in them for whether callers use its fields (``dktools
structs``), so a handle's or an unnamed struct's body fails unless
tools/structs-allow.txt names it.

The compile database is DIR's ``compile_commands.json`` (``--build``,
e.g. a verify tree's binary dir), else ``build/PRESET``'s.  Exits 0 when
every header compiles or is allowed, no allow line is stale and every
handle is tagged to match its calls, 1 otherwise, 2 for a missing
database or a malformed allow line.
"""

from __future__ import annotations

import shlex
import tempfile
from dataclasses import dataclass
from pathlib import Path

from . import handles, irdups, procs, structs
from .gitutil import ToolError
from .retoken import VENDORED, sources

ALLOW_FILE = Path("tools/headers-allow.txt")
#: The unit whose compile command every header is checked with
REFERENCE_UNIT = "src/datakit.c"
COMPILE_TIMEOUT = 120.0
STALL_TIMEOUT = 600.0


@dataclass(frozen=True)
class Failure:
    path: str
    error: str


def header_command(command: str, unit: Path) -> list[str]:
    """The reference compile command made to check 'unit' (a file that
    includes one header): its output, dependency and warning-as-error
    flags dropped, the source replaced, syntax only"""
    argv = shlex.split(command)
    kept: list[str] = []
    skip = False
    for arg in argv:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if arg in ("-c", "-MD", "-MMD") or arg.startswith("-Werror"):
            continue
        if arg.endswith(".c"):
            continue
        kept.append(arg)
    return [*kept, "-fsyntax-only", "-x", "c", str(unit)]


def parse_allow(text: str) -> dict[str, str]:
    """The allow file's path -> reason entries; ToolError for a line
    without a reason"""
    allowed: dict[str, str] = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            raise ToolError(f"{ALLOW_FILE}:{n}: PATH REASON needs a reason")
        allowed[parts[0]] = parts[1]
    return allowed


def first_error(stderr: str) -> str:
    """The first ``error:`` line of a compiler's output (else its last
    line), for a one-line report"""
    lines = stderr.strip().splitlines()
    for line in lines:
        if "error:" in line:
            return line.strip()
    return lines[-1].strip() if lines else "compile failed"


def _headers(repo: Path) -> list[str]:
    """The tracked src headers in a git checkout; every src header in an
    exported tree (a gate's copy, which is not a checkout)"""
    if (repo / ".git").exists():
        return [p for p in sources(repo) if p.endswith(".h")]
    found = [
        p.relative_to(repo).as_posix()
        for p in (repo / "src").rglob("*.h")
        if p.is_file()
    ]
    return sorted(p for p in found if not p.startswith(VENDORED))


def _check_one(item: tuple[str, str, str, str]) -> Failure | None:
    path, header, directory, command = item
    with tempfile.TemporaryDirectory(prefix="dktools-headers-") as tmp:
        unit = Path(tmp) / "unit.c"
        unit.write_text(f'#include "{header}"\n', encoding="utf-8")
        proc = procs.run(
            header_command(command, unit), cwd=Path(directory), timeout=COMPILE_TIMEOUT
        )
    if proc.returncode == 0:
        return None
    return Failure(path, first_error(proc.stderr.decode(errors="replace")))


def _reference(repo: Path, build: Path | None, preset: str) -> tuple[str, str]:
    if build is None:
        entries = irdups.database_entries(repo, preset)
    else:
        entries = irdups.database_entries_at(repo, build / "compile_commands.json")
    if REFERENCE_UNIT not in entries:
        raise ToolError(f"{REFERENCE_UNIT}: not in the compile database")
    return entries[REFERENCE_UNIT]


def run(
    repo: Path,
    paths: tuple[str, ...],
    build: Path | None,
    preset: str,
    jobs: int | None = None,
) -> int:
    allow_path = repo / ALLOW_FILE
    allowed = parse_allow(
        allow_path.read_text(encoding="utf-8") if allow_path.exists() else ""
    )
    files = list(paths) or _headers(repo)
    if not files:
        # A gate that checked nothing would pass without checking anything
        raise ToolError(f"{repo}: no headers found under src/")
    directory, command = _reference(repo, build, preset)
    work = [(p, str((repo / p).resolve()), directory, command) for p in files]
    results = procs.parallel_map(
        _check_one,
        work,
        jobs=procs.sizing(jobs).build,
        stall_timeout=STALL_TIMEOUT,
        describe=lambda item: item[0],
        noun="header",
    )
    failed = [f for f in results if f is not None]
    bad = [f for f in failed if f.path not in allowed]
    for f in bad:
        print(f"{f.path}: does not compile on its own: {f.error}")

    # An allow line is stale when its header compiles on its own now, or
    # names no header that was checked (and all of them were)
    checked = set(files)
    failing = {f.path for f in failed}
    stale = [
        p
        for p in allowed
        if (p in checked and p not in failing) or (not paths and p not in checked)
    ]
    for p in stale:
        print(f"{ALLOW_FILE}: {p}: allowed but compiles on its own or is no header")

    found = handles.collect(repo, paths)
    untagged = [p for h in found for p in h.problems()]
    for p in untagged:
        print(p)
    shown, bodies = structs.gate(repo, paths)
    for p in shown:
        print(p)

    print(
        f"headers: {len(bad)} to fix, {len(failed) - len(bad)} allowed, "
        f"{len(stale)} stale allow lines, of {len(files)} headers; "
        f"{len(untagged)} of {len(found)} handles to fix; "
        f"{len(shown)} of {bodies} struct bodies to fix"
    )
    return 1 if bad or stale or untagged or shown else 0

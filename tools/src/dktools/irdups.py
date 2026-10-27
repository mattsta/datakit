"""``dktools irdups``: functions that compile to the same code.

``dktools irdups [--preset PRESET] [--min-instructions N] [FILE ...]``

Compiles each source of a preset build's compile database (default
``build/dev``, made by ``dktools configure``) to LLVM IR at ``-O1
-fno-inline`` and compares function bodies after the optimizer has
erased spelling: renamed variables, reordered declarations, macro
versus function, ``for`` versus ``while``.  ``dupfuncs --shape``
compares tokens; this finds what only the compiler can see is the same.

A body is normalised before it is hashed: debug and other metadata,
attribute groups and value names go (locals and labels are numbered by
first use), string constants read as their contents (not their
numbering), and the names of
called functions and referenced globals stay, so two bodies match only
when they compute the same thing from the same callees.  Bodies of at
least ``--min-instructions`` instructions (default 20) found under two
or more function names are listed, the largest first.  One function
compiled into many units (a header's static inline) has one name and is
not listed.

Each file is compiled with the database's command for it (the test
target's when there is one, so test code is compared too), minus its
output, optimisation, debug and sanitizer flags.  Files default to
every database entry under src outside vendored code.  Exits 0 (the
report is advice), 2 for a missing database or a file it lacks.
"""

from __future__ import annotations

import hashlib
import json
import re
import shlex
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from . import procs
from .gitutil import ToolError
from .retoken import VENDORED

#: one unit's compile to IR stops after this long
COMPILE_TIMEOUT = 300.0
#: no unit finishing for this long fails the run
STALL_TIMEOUT = 600.0

_DEFINE = re.compile(r"^define\b[^@]*@(\"[^\"]+\"|[\w.$]+)\(")
_METADATA = re.compile(r",?\s*!(?:[\w.]+)\s+!\d+")
_ATTR_GROUP = re.compile(r"\s#\d+")
_LOCAL = re.compile(r"%(\"[^\"]+\"|[\w.$]+)")
_STR_CONST = re.compile(r"@\.str(?:\.\d+)?\b")
_STR_DEF = re.compile(r"^(@\.str(?:\.\d+)?) = .*?(c\".*\")", re.MULTILINE)
_SUFFIX = re.compile(r"\.\d+$")
_DROPPED_FLAGS = re.compile(r"^-(?:O\w*|g\w*|fsanitize\S*|fno-sanitize\S*|c|MD|MMD)$")


@dataclass(frozen=True)
class Body:
    unit: str
    name: str
    instructions: int
    digest: str


def function_bodies(ir: str) -> list[tuple[str, list[str]]]:
    """Each defined function's name and the lines of its body."""
    found: list[tuple[str, list[str]]] = []
    name: str | None = None
    lines: list[str] = []
    for line in ir.splitlines():
        if name is None:
            m = _DEFINE.match(line)
            if m:
                name, lines = m.group(1).strip('"'), [line]
            continue
        lines.append(line)
        if line == "}":
            found.append((name, lines))
            name = None
    return found


def normalise(
    lines: list[str], strings: dict[str, str] | None = None
) -> tuple[str, int]:
    """The body as compared, and its instruction count.  The define line
    keeps only its parameter list (the return type shows in each ret);
    comments, metadata, attribute groups and value names go.  A string
    constant reads as its contents ('strings': @.str.N -> c"..."), so
    its numbering in the unit does not count and its text does."""
    strings = strings or {}
    numbers: dict[str, str] = {}

    def text(m: re.Match[str]) -> str:
        return "@str" + strings.get(m.group(0), "?")

    def local(m: re.Match[str]) -> str:
        return numbers.setdefault(m.group(1), f"%v{len(numbers)}")

    out: list[str] = []
    count = 0
    for i, raw in enumerate(lines):
        line = raw.split(";", 1)[0].rstrip()
        if i == 0:
            m = _DEFINE.match(line)
            assert m is not None
            rest = line[m.end() :]
            line = f"define({rest[: rest.rfind(')')]})"
        line = _METADATA.sub("", line)
        line = _ATTR_GROUP.sub("", line)
        if not line.strip():
            continue
        if i and not line.startswith(" ") and line.endswith(":"):
            # a label, numbered with the locals that branch to it
            line = numbers.setdefault(line[:-1].strip('"'), f"%v{len(numbers)}")
            out.append(line + ":")
            continue
        if i and line.startswith(" "):
            count += 1
        line = _LOCAL.sub(local, line)
        out.append(_STR_CONST.sub(text, line).strip())
    return "\n".join(out), count


_TYPE_TOKEN = re.compile(r"(?:[IU](?:8|16|32|64|128)|Float|Double)(?=[A-Z_]|$)")


def template_twins(group: list[Body]) -> bool:
    """Whether a group's names differ only in a type token (I16, U64,
    Float, ...): instantiations of one type template, one source"""
    return len({_TYPE_TOKEN.sub("T", b.name) for b in group}) == 1


def _base_name(name: str) -> str:
    """A function's name without the .N a unit adds to a static copy"""
    return _SUFFIX.sub("", name)


def bodies_of(unit: str, ir: str, min_instructions: int) -> list[Body]:
    found = []
    strings = dict(_STR_DEF.findall(ir))
    for name, lines in function_bodies(ir):
        text, count = normalise(lines, strings)
        if count >= min_instructions:
            digest = hashlib.blake2b(text.encode(), digest_size=16).hexdigest()
            found.append(Body(unit, _base_name(name), count, digest))
    return found


def groups(bodies: list[Body]) -> list[list[Body]]:
    """Bodies sharing a digest under two or more names, largest first;
    within a group one entry per (unit, name)"""
    by_digest: dict[str, dict[tuple[str, str], Body]] = defaultdict(dict)
    for b in bodies:
        by_digest[b.digest].setdefault((b.unit, b.name), b)
    found = [
        sorted(members.values(), key=lambda b: (b.unit, b.name))
        for members in by_digest.values()
        if len({b.name for b in members.values()}) >= 2
    ]
    found.sort(key=lambda g: (-g[0].instructions, g[0].unit, g[0].name))
    return found


def ir_command(command: str) -> list[str]:
    """The database's compile command made to print the unit's IR"""
    argv = shlex.split(command)
    kept: list[str] = []
    skip = False
    for arg in argv:
        if skip:
            skip = False
            continue
        if arg == "-o":
            skip = True
            continue
        if _DROPPED_FLAGS.match(arg):
            continue
        kept.append(arg)
    return [*kept, "-O1", "-fno-inline", "-g0", "-S", "-emit-llvm", "-o", "-"]


def database_entries(repo: Path, preset: str) -> dict[str, tuple[str, str]]:
    """Repo-relative source -> (directory, command), the test target's
    command where a file has one"""
    db = repo / "build" / preset / "compile_commands.json"
    if not db.is_file():
        raise ToolError(f"{db}: no compile database (dktools configure {preset})")
    entries: dict[str, tuple[str, str]] = {}
    for entry in json.loads(db.read_text()):
        try:
            rel = Path(entry["file"]).resolve().relative_to(repo.resolve()).as_posix()
        except ValueError:
            continue
        if not rel.startswith("src/") or rel.startswith(VENDORED):
            continue
        command = entry.get("command") or shlex.join(entry["arguments"])
        if rel not in entries or "DATAKIT_TEST" in command:
            entries[rel] = (entry["directory"], command)
    return entries


def _compile_one(item: tuple[str, str, str, int]) -> list[Body]:
    unit, directory, command, min_instructions = item
    proc = procs.run(ir_command(command), cwd=Path(directory), timeout=COMPILE_TIMEOUT)
    if proc.returncode != 0:
        err = proc.stderr.decode(errors="replace").strip().splitlines()
        raise ToolError(f"{unit}: {err[-1] if err else 'compile failed'}")
    return bodies_of(unit, proc.stdout.decode(errors="replace"), min_instructions)


def run(
    repo: Path,
    paths: tuple[str, ...],
    preset: str,
    min_instructions: int,
    jobs: int | None = None,
) -> int:
    entries = database_entries(repo, preset)
    units = list(paths) or sorted(entries)
    missing = [u for u in units if u not in entries]
    if missing:
        raise ToolError(f"{missing[0]}: not in build/{preset}/compile_commands.json")
    work = [(u, *entries[u], min_instructions) for u in units]
    results = procs.parallel_map(
        _compile_one,
        work,
        jobs=procs.sizing(jobs).build,
        stall_timeout=STALL_TIMEOUT,
        describe=lambda item: item[0],
        noun="unit",
    )
    found = groups([b for unit_bodies in results for b in unit_bodies])
    twins = [g for g in found if template_twins(g)]
    for g in found:
        note = " (one type template)" if template_twins(g) else ""
        print(f"{g[0].instructions} instructions, {len(g)} copies{note}:")
        for b in g:
            print(f"  {b.unit}: {b.name}")
    print(
        f"irdups: {len(found) - len(twins)} groups, {len(twins)} type-template "
        f"twin groups, in {len(units)} units"
    )
    return 0

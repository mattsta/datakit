"""``dktools docexamples``: the docs' C examples call datakit as it is.

``dktools docexamples [--build DIR | --preset P] [-j N] [DOC ...]``

Every fenced ``c`` block in ``docs/**/*.md`` and ``README.md`` (or the
named DOCs) is compiled, syntax only, with the compile command the
build uses for ``src/datakit.c`` (as ``dktools headers``).  A block is
compiled after the public headers that declare the datakit calls it
makes or defining the ALL_CAPS constants it names, and the C headers
examples lean on; one with a definition, an
``#include`` or a declaration at its left margin is compiled as a file,
any other (a fragment) as the body of a function.  A part of a block
that shows what not to do (from a ``WRONG``, ``BAD`` or ``AVOID``
comment to a ``RIGHT``, ``GOOD`` or ``USE`` one) is not checked.

Examples are fragments: their variables are often the reader's, so an
undeclared identifier says nothing.  What is reported is what proves an
example does not match datakit: a call no header declares, a call with
too many or too few arguments, an argument of the wrong type, a member
the struct does not have, and an ALL_CAPS datakit constant that does
not exist.  Each is printed at its doc line.

The names the docs show that no source file has (``dktools docgaps
--stale``: removed calls, missing source files) are reported too, and
every relative link: its file must exist and its ``#anchor`` must be one
of that file's headings (GitHub's slugs).  This is the one docs gate
verify and validate run.

Exits 0 when no example shows such an error and no doc names a missing
call, 1 otherwise.
"""

from __future__ import annotations

import json
import re
import shlex
import tempfile
from dataclasses import dataclass
from pathlib import Path

from . import procs
from .coverage import collect_headers, declared_functions, inline_functions
from .docgaps import _source_words, api_text, stale_calls
from .gitutil import ToolError
from .headers import header_command

COMPILE_TIMEOUT = 120.0
STALL_TIMEOUT = 600.0

#: A fenced C block in a doc: its body starts on the line after the fence
_C_BLOCK = re.compile(r"^```c[ \t]*\n(.*?)^```", re.MULTILINE | re.DOTALL)

#: A call in a block
_CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")

#: A line that only makes sense at file scope: a preprocessor line, a
#: type definition, or a function definition or declaration at the left
#: margin
_FILE_SCOPE = re.compile(
    r"^(?:#\s*(?:include|define)\b|typedef\b|struct\s+\w+\s*\{|enum\s+\w*\s*\{"
    r"|static\b|[A-Za-z_][\w \t\*]*\b[A-Za-z_]\w*\s*\([^;]*\)\s*\{?\s*$)",
    re.MULTILINE,
)

#: The C headers examples use without including them
_LIBC = (
    "inttypes.h",
    "stdbool.h",
    "stddef.h",
    "stdint.h",
    "stdio.h",
    "stdlib.h",
    "string.h",
    "pthread.h",
)

#: A compiler error line in the unit
_ERROR = re.compile(r"^.*?unit\.c:(\d+):\d+: error: (.*)$", re.MULTILINE)

#: The errors that prove an example stale
_PROOF = re.compile(
    r"call to undeclared function|implicit declaration of function"
    r"|too (?:many|few) arguments to function call"
    r"|incompatible .*(?:passing|initializing|assigning)"
    r"|passing .* to parameter of incompatible type"
    r"|no member named"
)

#: A function passed where a data pointer goes: an undeclared variable of
#: the reader's that happens to share a libc function's name (``lock``)
_FUNCTION_FOR_DATA = re.compile(
    r"incompatible pointer types passing '[^']*\([^']*\)' "
    r"(?:\(aka '[^']*'\) )?to parameter of type '(?![^']*\()"
)

#: An undeclared identifier: an ALL_CAPS constant, or a camelCase function
#: (C23 has no implicit declarations, so a call to a name no header
#: declares is reported this way)
_UNDECLARED = re.compile(r"use of undeclared identifier '([A-Za-z_][A-Za-z0-9_]*)'")

#: The leading lowercase run of a name (``flex`` of ``flexNew``)
_STEM = re.compile(r"^[a-z][a-z0-9]*")


def stale_name(
    name: str,
    prefixes: frozenset[str],
    stems: frozenset[str],
    source: frozenset[str] = frozenset(),
) -> bool:
    """Whether an undeclared 'name' is a datakit name no source file has:
    an ALL_CAPS constant with a datakit prefix, or a camelCase name
    starting like a public function (a variable of the reader's is
    neither; a name in 'source', every word of src/, is one an excerpt of
    the implementation uses, not a stale one)."""
    if name in source:
        return False
    if re.fullmatch(r"[A-Z][A-Z0-9_]+", name):
        return name.split("_")[0] in prefixes
    stem = _STEM.match(name)
    return (
        bool(stem)
        and "_" not in name
        and stem.group(0) != name
        and (stem.group(0) in stems)
    )


@dataclass(frozen=True)
class Block:
    doc: str
    line: int  # the doc line of the block's first line
    code: str


@dataclass(frozen=True)
class Finding:
    doc: str
    line: int
    message: str
    detail: str = ""  # the compiler's whole output for the unit


#: A markdown link: [text](target)
_LINK = re.compile(r"\[[^\]\n]*\]\(([^)\s]+)\)")

#: A heading line
_HEADING = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$", re.MULTILINE)


def slug(heading: str) -> str:
    """GitHub's anchor for a heading: lowercase, markup and punctuation
    dropped (hyphens and underscores kept), each space a hyphen."""
    text = re.sub(r"[`*]", "", heading).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(text: str) -> set[str]:
    """The anchors a doc's headings give (fenced code left out), a
    repeated heading numbered as GitHub numbers it."""
    prose = re.sub(r"^```.*?^```", "", text, flags=re.MULTILINE | re.DOTALL)
    seen: dict[str, int] = {}
    out: set[str] = set()
    for m in _HEADING.finditer(prose):
        base = slug(m.group(1))
        n = seen.get(base, 0)
        out.add(base if n == 0 else f"{base}-{n}")
        seen[base] = n + 1
    return out


def broken_links(repo: Path, paths: list[Path]) -> list[Finding]:
    """Relative links in 'paths' whose file does not exist, or whose
    anchor is no heading of the file."""
    out: list[Finding] = []
    cache: dict[Path, set[str]] = {}
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        prose = re.sub(
            r"^```.*?^```",
            lambda m: "\n" * m.group(0).count("\n"),
            text,
            flags=re.MULTILINE | re.DOTALL,
        )
        rel = path.relative_to(repo).as_posix()
        for m in _LINK.finditer(prose):
            target = m.group(1)
            if re.match(r"[a-z]+:", target) or target.startswith("<"):
                continue  # a URL
            file, _, anchor = target.partition("#")
            dest = (path.parent / file).resolve() if file else path.resolve()
            line = prose.count("\n", 0, m.start()) + 1
            if not dest.exists():
                out.append(Finding(rel, line, f"link to a missing file: {target}"))
                continue
            if anchor and dest.suffix == ".md":
                if dest not in cache:
                    cache[dest] = anchors(dest.read_text(encoding="utf-8"))
                if anchor not in cache[dest]:
                    out.append(
                        Finding(rel, line, f"link to a missing heading: {target}")
                    )
    return out


def blocks(repo: Path, docs: tuple[str, ...] = ()) -> list[Block]:
    """Every fenced C block of the docs (or of 'docs')."""
    if docs:
        paths = [repo / d for d in docs]
    else:
        paths = sorted((repo / "docs").rglob("*.md"))
        if (repo / "README.md").exists():
            paths.append(repo / "README.md")
    out: list[Block] = []
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(repo).as_posix()
        for m in _C_BLOCK.finditer(text):
            line = text.count("\n", 0, m.start(1)) + 1
            out.append(Block(rel, line, m.group(1)))
    return out


def declaring_headers(headers: dict[str, str]) -> dict[str, str]:
    """Each public function's header (an include path under src/)."""
    out: dict[str, str] = {}
    for path, text in headers.items():
        include = path.removeprefix("src/")
        for name in declared_functions(api_text(text)):
            out.setdefault(name, include)
    return out


#: A name a header defines: a macro of any case, an ALL_CAPS enum
#: constant, or a typedef's name
_DEFINES = re.compile(
    r"^\s*#\s*define\s+([A-Za-z_]\w*)"
    r"|^\s*([A-Z][A-Z0-9_]+)\s*(?:=|,|$)"
    r"|^\s*typedef\b[^;{}]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;"
    r"|\}\s*([A-Za-z_]\w*)\s*;",
    re.MULTILINE,
)

#: A word in a block
_WORD = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def defining_headers(headers: dict[str, str]) -> dict[str, str]:
    """Each name's defining header (an include path under src/): its
    macros, ALL_CAPS enum constants, typedef names and inline
    functions."""
    out: dict[str, str] = {}
    for path, text in headers.items():
        include = path.removeprefix("src/")
        for m in _DEFINES.finditer(text):
            name = next(g for g in m.groups() if g)
            out.setdefault(name, include)
        for name in inline_functions(text):
            out.setdefault(name, include)
    return out


def constant_prefixes(headers: dict[str, str]) -> frozenset[str]:
    """The prefixes of the ALL_CAPS names the public headers define
    (``FLEX_`` of ``FLEX_ENDPOINT_TAIL``)."""
    words = re.compile(r"\b([A-Z][A-Z0-9]+)_[A-Z0-9_]+\b")
    return frozenset(
        m.group(1) for text in headers.values() for m in words.finditer(text)
    )


def unit_text(
    block: Block, declared: dict[str, str], defined: dict[str, str] | None = None
) -> tuple[str, int]:
    """The unit compiling 'block', and the unit line of the block's
    first line: after the headers declaring or defining the names it
    uses."""
    defined = defined or {}
    words = set(_WORD.findall(block.code))
    needed = sorted(
        {declared[n] for n in words if n in declared}
        | {defined[n] for n in words if n in defined}
        | {"datakit.h"}
    )
    prelude = [f"#include <{h}>" for h in _LIBC]
    prelude += [f'#include "{h}"' for h in needed]
    if _FILE_SCOPE.search(block.code):
        return "\n".join([*prelude, block.code]), len(prelude) + 1
    lines = [*prelude, "static void dkDocExample_(void) {", block.code, "}"]
    return "\n".join(lines), len(prelude) + 2


#: A comment opening an example of what not to do, and one closing it
_WRONG = re.compile(r"/[*/].*\b(?:WRONG|BAD|AVOID|DON'T|NEVER|UNSAFE)\b")
_RIGHT = re.compile(r"/[*/].*\b(?:RIGHT|GOOD|CORRECT|USE|SAFE|BETTER|INSTEAD)\b")


def wrong_lines(code: str) -> set[int]:
    """The block's lines (1-based) that show what not to do: from a
    comment saying WRONG, BAD, AVOID, ... up to one saying RIGHT, GOOD,
    USE, ... or the block's end.  Errors there are the example's point."""
    out: set[int] = set()
    wrong = False
    for number, line in enumerate(code.splitlines(), 1):
        if _RIGHT.search(line):
            wrong = False
        elif _WRONG.search(line):
            wrong = True
        if wrong:
            out.add(number)
    return out


def proofs(
    stderr: str,
    block: Block,
    first: int,
    prefixes: frozenset[str],
    stems: frozenset[str] = frozenset(),
    source: frozenset[str] = frozenset(),
) -> list[Finding]:
    """The errors in 'stderr' that prove 'block' stale, at doc lines (not
    in a part of the block that shows what not to do)."""
    out: list[Finding] = []
    wrong = wrong_lines(block.code)
    code_lines = block.code.splitlines()
    for m in _ERROR.finditer(stderr):
        unit_line, message = int(m.group(1)), m.group(2).strip()
        if unit_line - first + 1 in wrong:
            continue
        at = unit_line - first
        said = code_lines[at] if 0 <= at < len(code_lines) else ""
        undeclared = _UNDECLARED.search(message)
        if _FUNCTION_FOR_DATA.search(message):
            continue
        name = undeclared.group(1) if undeclared else ""
        stale = _PROOF.search(message) or (
            undeclared
            and stale_name(name, prefixes, stems, source)
            # a camelCase name is a call the line makes; a variable of the
            # reader's that shares a prefix is not
            and (name.isupper() or re.search(rf"\b{re.escape(name)}\s*\(", said))
        )
        if stale and unit_line >= first:
            out.append(Finding(block.doc, block.line + unit_line - first, message))
    return out


def _check(
    item: tuple[
        Block, str, int, str, str, frozenset[str], frozenset[str], frozenset[str]
    ],
) -> list[Finding]:
    block, unit, first, directory, command, prefixes, stems, source = item
    with tempfile.TemporaryDirectory(prefix="dktools-docexamples-") as tmp:
        path = Path(tmp) / "unit.c"
        path.write_text(unit, encoding="utf-8")
        argv = [*header_command(command, path), "-ferror-limit=0"]
        if "clang" in Path(argv[0]).name:
            # No typo correction: an undeclared helper of the reader's
            # would be checked against whatever name it is closest to
            argv.append("-fno-spell-checking")
        proc = procs.run(argv, cwd=Path(directory), timeout=COMPILE_TIMEOUT)
    stderr = proc.stderr.decode(errors="replace")
    found = proofs(stderr, block, first, prefixes, stems, source)
    if found:
        numbered = "\n".join(
            f"{n:4d}  {line}" for n, line in enumerate(unit.splitlines(), 1)
        )
        found[0] = Finding(
            found[0].doc, found[0].line, found[0].message, f"{numbered}\n{stderr}"
        )
    return found


def reference(repo: Path, build: Path | None, preset: str) -> tuple[str, str]:
    """The directory and command the build compiles src/datakit.c with,
    from DIR's compile_commands.json (a verify tree's binary dir, whose
    sources are its own export) or build/PRESET's."""
    db = (build or repo / "build" / preset) / "compile_commands.json"
    if not db.is_file():
        raise ToolError(f"{db}: no compile database")
    best: tuple[str, str] | None = None
    for entry in json.loads(db.read_text()):
        if not entry["file"].endswith("/src/datakit.c"):
            continue
        command = entry.get("command") or shlex.join(entry["arguments"])
        if best is None or "DATAKIT_TEST" in command:
            best = (entry["directory"], command)
    if best is None:
        raise ToolError(f"{db}: src/datakit.c is not in it")
    return best


def run(
    repo: Path,
    docs: tuple[str, ...],
    build: Path | None,
    preset: str,
    jobs: int | None = None,
    verbose: bool = False,
) -> int:
    found = blocks(repo, docs)
    if not found:
        raise ToolError(f"{repo}: no C examples found")
    headers = collect_headers(repo)
    # Every header's functions and macros (ctest.h's TEST and ERR, the
    # template headers' calls), the public ones first
    every = {
        p: t
        for p, t in collect_headers(repo, public_only=False).items()
        if not p.startswith("src/deps")
    }
    declared = {**declaring_headers(every), **declaring_headers(headers)}
    defined = defining_headers(every)
    prefixes = constant_prefixes(headers)
    source = frozenset(_source_words(repo))
    stems = frozenset(
        m.group(0)
        for text in headers.values()
        for name in declared_functions(api_text(text))
        if (m := _STEM.match(name)) and len(m.group(0)) > 2
    )
    directory, command = reference(repo, build, preset)
    work = []
    for block in found:
        unit, first = unit_text(block, declared, defined)
        work.append((block, unit, first, directory, command, prefixes, stems, source))
    results = procs.parallel_map(
        _check,
        work,
        jobs=procs.sizing(jobs).build,
        stall_timeout=STALL_TIMEOUT,
        describe=lambda item: f"{item[0].doc}:{item[0].line}",
        noun="example",
    )
    findings = [f for r in results for f in r]
    for f in findings:
        print(f"{f.doc}:{f.line}: {f.message}")
        if verbose and f.detail:
            print(f.detail)
    names = [] if docs else stale_calls(repo, headers)
    for c in names:
        print(f"{c.doc}:{c.line}: {c.name}: no source file names it")
    doc_paths = (
        [repo / d for d in docs]
        if docs
        else sorted((repo / "docs").rglob("*.md")) + [repo / "README.md"]
    )
    links = broken_links(repo, [p for p in doc_paths if p.exists()])
    for f in links:
        print(f"{f.doc}:{f.line}: {f.message}")
    stale = len(
        {f.doc for f in findings} | {c.doc for c in names} | {f.doc for f in links}
    )
    print(
        f"docexamples: {len(findings)} errors proving an example stale, "
        f"{len(names)} names no source has, {len(links)} broken links, in "
        f"{stale} docs; {len(found)} C examples checked"
    )
    return 1 if findings or names or links else 0

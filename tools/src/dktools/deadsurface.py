"""``dktools deadsurface``: settings and choices the library never acts on.

``dktools deadsurface``

A public surface is dead when callers can set it but no library code
reads it: a ``*Config`` / ``*Params`` / ``*Options`` struct field the
library only ever writes (defaults, initializers), or an enum constant
a public header declares that no library code names outside its own
definition (a mode no code handles, a result no code returns).  Such a
surface is half a feature: either the code that acts on it is missing,
or the surface should go.

Library code is every ``.c`` and ``.h`` under src, without test files
(``*Test.c``, ``datakit-test.c``, ``src/fuzz/``), ``#ifdef DATAKIT_TEST``
blocks, comments and preprocessor lines, and vendored code.  A read of a
field is ``.f`` or ``->f`` that is not the left side of a plain ``=`` and
not a designated initializer.  A report: exit 0.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .coverage import code_text, collect_headers
from .docgaps import api_text
from .retoken import VENDORED
from .structs import bodies_in

#: Struct names whose fields are settings a caller fills
_SETTINGS = re.compile(r"(Config|Params|Options)$")
#: 'enum [tag] {' at a line start, possibly after typedef
_ENUM = re.compile(r"^\s*(?:typedef\s+)?enum\b[^{;]*\{", re.MULTILINE)
_IDENT = re.compile(r"[A-Za-z_]\w*")


@dataclass(frozen=True)
class Dead:
    header: str
    kind: str  # "field" or "enum"
    owner: str  # struct name, or "enum" for a constant
    name: str


def _is_test_file(rel: str) -> bool:
    return (
        rel.endswith("Test.c")
        or rel.endswith("/datakit-test.c")
        or rel.startswith("src/fuzz/")
        or "/deps/" in rel
        or rel.startswith(VENDORED)
    )


def library_text(repo: Path) -> str:
    """Every library source under src as one text, code only."""
    parts: list[str] = []
    for path in sorted((repo / "src").rglob("*")):
        if path.suffix not in (".c", ".h") or not path.is_file():
            continue
        rel = path.relative_to(repo).as_posix()
        if _is_test_file(rel):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        parts.append(code_text(api_text(text)))
    return "\n".join(parts)


def _top_level_statements(body: str) -> list[str]:
    """The ';'-separated declarations of a struct body at its own depth
    (a nested struct or union counts as one statement)."""
    out: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(body):
        depth += ch == "{"
        depth -= ch == "}"
        if ch == ";" and depth == 0:
            out.append(body[start:i])
            start = i + 1
    return out


def field_names(body: str) -> list[str]:
    """The field names a struct body declares (bit-field widths and array
    bounds dropped; a nested aggregate's own fields are not listed)."""
    names: list[str] = []
    for stmt in _top_level_statements(body):
        if "{" in stmt or "(" in stmt:
            continue  # nested aggregate or function pointer
        for decl in stmt.split(","):
            decl = re.sub(r"\[.*?\]", "", decl.split(":")[0])
            idents = _IDENT.findall(decl)
            if idents:
                names.append(idents[-1])
    return names


def enum_constants(text: str) -> list[str]:
    """The constants of every enum body in 'text' (code only)."""
    out: list[str] = []
    for m in _ENUM.finditer(text):
        depth = 0
        for end in range(m.end() - 1, len(text)):
            depth += text[end] == "{"
            depth -= text[end] == "}"
            if depth == 0:
                break
        body = text[m.end() : end]
        for item in body.split(","):
            idents = _IDENT.findall(item.split("=")[0])
            if idents:
                out.append(idents[0])
    return out


def field_read(library: str, name: str) -> bool:
    """Whether 'library' reads a member named 'name' anywhere."""
    pattern = re.compile(r"(?:\.|->)\s*" + re.escape(name) + r"\b(?!\s*=[^=])")
    for m in pattern.finditer(library):
        before = library[: m.start()].rstrip()
        # '.name =' in an initializer list is excluded by the lookahead;
        # a '.name' starting a designator without '=' cannot occur
        if not before.endswith(("{", ",")) or m.group(0).startswith("->"):
            return True
    return False


def constant_named(library: str, name: str) -> bool:
    """Whether 'library' names 'name' besides its own definition."""
    return len(re.findall(r"\b" + re.escape(name) + r"\b", library)) > 1


def collect(repo: Path) -> list[Dead]:
    library = library_text(repo)
    out: list[Dead] = []
    for header, text in collect_headers(repo).items():
        if header.startswith(VENDORED):
            continue
        for struct in bodies_in(header, text):
            if not any(_SETTINGS.search(n) for n in struct.names):
                continue
            for field in field_names(struct.body):
                if not field_read(library, field):
                    out.append(Dead(header, "field", struct.name, field))
        for const in enum_constants(code_text(api_text(text))):
            if not constant_named(library, const):
                out.append(Dead(header, "enum", "enum", const))
    return out


def render(dead: list[Dead]) -> str:
    if not dead:
        return "deadsurface: none"
    lines = [f"deadsurface: {len(dead)} settings or constants no library code acts on"]
    for d in dead:
        where = f"{d.owner}.{d.name}" if d.kind == "field" else d.name
        lines.append(f"  {d.header}: {d.kind} {where}")
    return "\n".join(lines)


def run(repo: Path) -> int:
    print(render(collect(repo)))
    return 0

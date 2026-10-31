"""``dktools typeaudit``: what the public API's integers mean.

``dktools typeaudit [--concept WORD] [--min N] [HEADER ...]``

Lists every integer-typed parameter, return and struct field in the
public headers, grouped by the concept its name ends in (``timeoutMs``:
ms, ``nodeId``: id, ``maxLevels``: level), each with the spread of types
the API spells it with.  A concept with a unit, a range or an identity
of its own, spelled several ways or crossing several headers, is a
candidate for a typedef that says what the number is; a plain count or
length named for what it counts is not.  ``--concept`` lists every site
of one concept.  A report: exits 0, or 2 for a missing header.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .coverage import code_text, collect_headers
from .docgaps import api_text
from .gitutil import ToolError
from .handles import calls_in
from .retoken import VENDORED
from .structs import bodies_in

_INTEGER = (
    r"(?:unsigned\s+(?:long\s+long|long|int|short|char)|unsigned"
    r"|signed\s+char|long\s+long|long|short|int"
    r"|u?int(?:_fast|_least)?(?:8|16|32|64)_t|u?intmax_t|u?intptr_t"
    r"|size_t|ssize_t|ptrdiff_t|__u?int128_t)"
)
# One declaration: [const] TYPE [*...] [const] NAME [array]
_DECL = re.compile(
    r"^(?:const\s+|static\s+|volatile\s+)*(?P<type>" + _INTEGER + r")\b"
    r"\s*(?P<ptr>[\s*]*?)(?:const\s+)?(?P<name>[A-Za-z_]\w*)\s*"
    r"(?P<array>\[[^\]]*\])?\s*(?::\s*\w+)?$"
)
# A return type before the function's name
_RETURN = re.compile(
    r"^(?:\[\[[^\]]*\]\]\s*)*(?:(?:static|inline|extern|const|DK_\w+)\s+)*"
    r"(?P<type>" + _INTEGER + r")\s+(?P<name>[A-Za-z_]\w*)\s*\("
)
_WORDS = re.compile(r"[A-Z]+(?![a-z])|[A-Z]?[a-z0-9]+|[A-Z]")


@dataclass(frozen=True)
class Site:
    header: str
    owner: str  # the function or struct
    name: str  # the parameter or field (the function's own name: its return)
    type: str
    pointer: bool


@dataclass
class Group:
    concept: str
    sites: list[Site] = field(default_factory=list)
    types: dict[str, int] = field(default_factory=dict)


def concept(name: str) -> str:
    """The word a name ends in, lowercased, a plural made singular
    ('maxLevels': 'level'; 'ms', 'us', 'status' stay)."""
    words = [w for part in name.split("_") for w in _WORDS.findall(part)]
    last = (words[-1] if words else name).lower()
    if len(last) > 3 and last.endswith("s") and not last.endswith(("ss", "us")):
        last = last[:-1]
    return last


def _split_top(text: str, sep: str) -> list[str]:
    """'text' split at each 'sep' outside brackets and parentheses."""
    parts: list[str] = []
    depth = 0
    start = 0
    for i, c in enumerate(text):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts if p.strip()]


def _declared(text: str) -> tuple[str, str, bool] | None:
    m = _DECL.match(" ".join(text.split()))
    if not m:
        return None
    pointer = "*" in m.group("ptr") or m.group("array") is not None
    return " ".join(m.group("type").split()), m.group("name"), pointer


def prototype_sites(header: str, func: str, proto: str) -> list[Site]:
    """The integer return (named as the function) and parameters of one
    prototype."""
    sites: list[Site] = []
    one = " ".join(proto.split())
    ret = _RETURN.match(one)
    if ret and ret.group("name") == func:
        rest = one[ret.end() - 1 :]
        if not rest.startswith("(*"):
            sites.append(Site(header, func, func, ret.group("type"), False))
    open_ = one.find("(", one.find(func) if func in one else 0)
    if open_ < 0:
        return sites
    depth = 0
    close = -1
    for i in range(open_, len(one)):
        if one[i] == "(":
            depth += 1
        elif one[i] == ")":
            depth -= 1
            if not depth:
                close = i
                break
    for param in _split_top(one[open_ + 1 : close], ","):
        found = _declared(param)
        if found:
            sites.append(Site(header, func, found[1], found[0], found[2]))
    return sites


def field_sites(header: str, owner: str, body: str) -> list[Site]:
    """The integer fields of one struct body (nested bodies skipped)."""
    sites: list[Site] = []
    for decl in _split_top(body, ";"):
        if "{" in decl:
            continue
        head, *more = _split_top(decl, ",")
        found = _declared(head)
        if not found:
            continue
        type_, name, pointer = found
        sites.append(Site(header, owner, name, type_, pointer))
        for extra in more:
            m = re.match(r"^(\**)\s*([A-Za-z_]\w*)", extra)
            if m:
                sites.append(Site(header, owner, m.group(2), type_, bool(m.group(1))))
    return sites


def collect(repo: Path, wanted: tuple[str, ...] = ()) -> list[Site]:
    headers = {
        p: t for p, t in collect_headers(repo).items() if not p.startswith(VENDORED)
    }
    for path in wanted:
        if path not in headers:
            raise ToolError(f"{path}: not a public header")
    sites: list[Site] = []
    for path, text in headers.items():
        if wanted and path not in wanted:
            continue
        for func, proto in calls_in(text, set()):
            sites += prototype_sites(path, func, proto)
        for struct in bodies_in(path, text):
            sites += field_sites(path, struct.name, code_text(api_text(struct.body)))
    return sites


def group(sites: list[Site]) -> list[Group]:
    """Concepts by site count (most first), each with its type spread."""
    groups: dict[str, Group] = {}
    for s in sites:
        g = groups.setdefault(concept(s.name), Group(concept(s.name)))
        g.sites.append(s)
        g.types[s.type] = g.types.get(s.type, 0) + 1
    return sorted(groups.values(), key=lambda g: (-len(g.sites), g.concept))


def render(groups: list[Group], min_sites: int) -> str:
    lines: list[str] = []
    for g in groups:
        if len(g.sites) < min_sites:
            continue
        spread = ", ".join(
            f"{t} {n}" for t, n in sorted(g.types.items(), key=lambda kv: -kv[1])
        )
        headers = len({s.header for s in g.sites})
        lines.append(
            f"{g.concept}  {len(g.sites)} sites  {spread}  ({headers} headers)"
        )
    return "\n".join(lines) + "\n"


def render_concept(groups: list[Group], word: str) -> str:
    lines = []
    for g in groups:
        if g.concept != word:
            continue
        for s in sorted(g.sites, key=lambda s: (s.header, s.owner, s.name)):
            star = " *" if s.pointer else ""
            lines.append(f"{s.header}  {s.owner}  {s.type}{star} {s.name}")
    return "\n".join(lines) + "\n"


def run(repo: Path, wanted: tuple[str, ...], word: str | None, min_sites: int) -> int:
    groups = group(collect(repo, wanted))
    print(render_concept(groups, word) if word else render(groups, min_sites), end="")
    return 0

"""``dktools structs``: struct bodies public headers show but should not.

``dktools structs [HEADER ...] [--all]``

A public header shows a struct's fields only when callers work with
them: configurations they fill, results and statistics they read,
iterators they declare on the stack.  Two kinds of body break that:

``handle``
    A public call returns it by pointer (``T *tNew(void)``): callers
    hold it as a handle, so its fields are the module's own and belong
    in ``*Internal.h`` (or the ``.c``) behind ``typedef struct T T;``.
``unnamed``
    No public call names it, directly or through a struct that one
    does: it is implementation detail.

Public calls are the prototypes, inline definitions and function
pointers of every public header (test-only code aside).  ``--all`` also
lists the data structs.  A report: exit 0, 2 for a header that is not
public.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .coverage import code_text, collect_headers
from .docgaps import api_text
from .handles import calls_in
from .retoken import VENDORED

#: 'struct tag {' or 'typedef struct [tag] {' at the start of a line
_BODY = re.compile(r"^\s*(?:typedef\s+)?struct\s+(\w+)?\s*\{", re.MULTILINE)
#: An inline definition's signature, up to its body
_INLINE = re.compile(
    r"\b(?:static\s+inline|DK_INLINE_ALWAYS)\b([^;{}]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\))\s*\{"
)
_IDENT = re.compile(r"\b[A-Za-z_]\w*\b")

KINDS = ("handle", "unnamed", "data")


@dataclass
class Struct:
    header: str
    names: tuple[str, ...]
    body: str
    kind: str = "data"

    @property
    def name(self) -> str:
        return self.names[-1]


def _closing(text: str, start: int) -> int:
    """The index just past the '}' matching the '{' at text[start]."""
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return i + 1
    return len(text)


def bodies_in(path: str, text: str) -> list[Struct]:
    """The structs 'text' defines with a body at file scope, each named by
    its tag and its typedef name (either may be absent)."""
    code = code_text(api_text(text))
    out: list[Struct] = []
    pos = 0
    while (m := _BODY.search(code, pos)) is not None:
        end = _closing(code, m.end() - 1)
        names = [m.group(1)] if m.group(1) else []
        after = re.match(r"\s*(\w+)\s*;", code[end:])
        if after and m.group(0).lstrip().startswith("typedef"):
            names.append(after.group(1))
        if names:
            body = code[m.end() : end - 1]
            out.append(Struct(path, tuple(dict.fromkeys(names)), body))
        pos = end
    return out


def public_calls(text: str) -> list[tuple[str, str]]:
    """(name, signature on one line) for each call a public header
    declares: prototypes, function pointers and inline definitions."""
    calls = calls_in(text, set())
    code = code_text(api_text(text))
    for m in _INLINE.finditer(code):
        calls.append((m.group(2), " ".join(m.group(1).split())))
    return calls


def classify(structs: list[Struct], headers: dict[str, str]) -> None:
    by_name = {n: s for s in structs for n in s.names}
    named: set[str] = set()
    returned: set[str] = set()
    for text in headers.values():
        for func, sig in public_calls(text):
            named |= set(_IDENT.findall(sig)) & by_name.keys()
            ret = re.match(
                rf"^(?:[\w\[\]]+\s+)*?(?:struct\s+)?(\w+)\s*\*\s*{re.escape(func)}\s*\(",
                sig,
            )
            if ret and ret.group(1) in by_name:
                returned.add(ret.group(1))
    # A struct a named struct holds (by value or pointer) is named too
    reached = {id(by_name[n]) for n in named}
    todo = [by_name[n] for n in named]
    while todo:
        s = todo.pop()
        for n in set(_IDENT.findall(s.body)) & by_name.keys():
            if id(by_name[n]) not in reached:
                reached.add(id(by_name[n]))
                todo.append(by_name[n])
    for s in structs:
        if any(n in returned for n in s.names):
            s.kind = "handle"
        elif id(s) not in reached:
            s.kind = "unnamed"


def collect(repo: Path, wanted: tuple[str, ...] = ()) -> list[Struct]:
    headers = {
        p: t for p, t in collect_headers(repo).items() if not p.startswith(VENDORED)
    }
    structs = [
        s
        for p, t in headers.items()
        if not wanted or p in wanted
        for s in bodies_in(p, t)
    ]
    classify(structs, headers)
    return sorted(structs, key=lambda s: (s.header, s.name))


def render(structs: list[Struct], show_all: bool) -> str:
    lines = [
        f"{s.header}: {s.name}: {s.kind}"
        for s in structs
        if show_all or s.kind != "data"
    ]
    counts = {k: sum(1 for s in structs if s.kind == k) for k in KINDS}
    lines.append(
        f"structs: {len(structs)} bodies in public headers: "
        + ", ".join(f"{counts[k]} {k}" for k in KINDS)
    )
    return "\n".join(lines)


def run(repo: Path, wanted: tuple[str, ...], show_all: bool) -> int:
    public = collect_headers(repo)
    unknown = [h for h in wanted if h not in public]
    if unknown:
        print(f"structs: not a public header: {', '.join(unknown)}")
        return 2
    print(render(collect(repo, wanted), show_all))
    return 0

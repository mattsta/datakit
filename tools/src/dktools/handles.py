"""``dktools handles``: every handle says whether its calls can move it.

``dktools handles [--list] [HEADER ...]``

A handle is a type the caller holds only by pointer: an opaque struct
(``typedef struct foo foo;`` with no body in a public header) or a
pointer-sized typedef (``typedef uint8_t flex;``, ``typedef char mds;``)
that carries a tag.  The tag, in the comment on the typedef's line or in
the comment block just above it, says what a caller's pointer does:

``@handle: moves``
    Calls that can move the object (it grows, shrinks or changes tier)
    take ``T **`` and write the new address back.  A ``T *`` call never
    moves it.  At least one public call takes ``T **``.
``@handle: returns``
    Calls that can move it return the new address; the caller stores the
    result.  At least one public call returns the handle and takes one.
``@handle: fixed``
    The address is fixed from creation to free.  No public call takes
    ``T **``, except to hand out a new one (a parameter named ``out``,
    ``*Out`` or ``*Ptr`` of a constructor or lookup).

Every opaque struct handle in a public header must carry a tag; a tagged
handle whose calls contradict it fails.  ``--list`` prints every handle
with its tag and its ``T **`` parameters.  Exits 0 when every handle is
tagged and consistent, 1 otherwise, 2 for a HEADER that is not public.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .coverage import code_text, collect_headers, declared_prototypes, template_types
from .docgaps import api_text

TAGS = ("moves", "returns", "fixed")

_TAG = re.compile(r"@handle:\s*(\w+)")
_OPAQUE = re.compile(r"^\s*typedef\s+struct\s+(\w+)\s+(\w+)\s*;", re.MULTILINE)
_SCALAR = re.compile(r"^\s*typedef\s+[\w\s]+?\b(\w+)\s*;", re.MULTILINE)
#: A parameter that hands out a new handle rather than moving one held
_OUT_PARAM = re.compile(r"^(out|\w*Out|\w*Ptr)$")
#: '(*name)(' opens a function pointer's parameter list
_FUNCTION_POINTER = re.compile(r"\(\s*\*\s*(\w+)\s*\)\s*\(")
#: Vendored headers keep their own conventions
VENDORED = "src/memshard/"


@dataclass
class Handle:
    name: str
    header: str
    tag: str | None
    opaque: bool
    #: "function(param)" for each 'T **param'
    moving: list[str] = field(default_factory=list)
    #: functions returning the handle and taking one
    returning: list[str] = field(default_factory=list)

    def problems(self) -> list[str]:
        where = f"{self.header}: {self.name}"
        if self.tag is None:
            return [f"{where}: no @handle tag (moves, returns or fixed)"]
        if self.tag not in TAGS:
            return [f"{where}: unknown tag '@handle: {self.tag}'"]
        if self.tag == "moves" and not self.moving:
            return [f"{where}: tagged moves, but no call takes {self.name} **"]
        if self.tag == "returns" and not self.returning:
            return [f"{where}: tagged returns, but no call returns a {self.name}"]
        if self.tag == "fixed":
            held = [m for m in self.moving if not _OUT_PARAM.match(_param(m))]
            if held:
                return [
                    f"{where}: tagged fixed, but {', '.join(held)} take {self.name} **"
                ]
        return []


def _param(moving: str) -> str:
    return moving[moving.index("(") + 1 : -1]


def _tag_near(text: str, start: int) -> str | None:
    """The @handle tag on the typedef's line, or in the comment block
    that ends on the line just above it."""
    line_end = text.find("\n", start)
    line = text[text.rfind("\n", 0, start) + 1 : line_end if line_end >= 0 else None]
    found = _TAG.search(line)
    if found:
        return found.group(1)
    before = text[: text.rfind("\n", 0, start) + 1].rstrip()
    if before.endswith("*/"):
        block = before[before.rfind("/*") :]
        found = _TAG.search(block)
        if found:
            return found.group(1)
    return None


def handles_in(path: str, text: str) -> list[Handle]:
    """The handles 'text' (the header at 'path') declares: opaque struct
    typedefs without a body here, and any typedef carrying a tag."""
    out: list[Handle] = []
    seen: set[str] = set()
    for m in _OPAQUE.finditer(text):
        struct, name = m.group(1), m.group(2)
        if re.search(rf"\bstruct\s+{re.escape(struct)}\s*\{{", text):
            continue  # its fields are in the header: a data struct
        out.append(Handle(name, path, _tag_near(text, m.start()), True))
        seen.add(name)
    for m in _SCALAR.finditer(text):
        name = m.group(1)
        if name in seen or "struct" in m.group(0) or "enum" in m.group(0):
            continue
        tag = _tag_near(text, m.start())
        if tag is not None:
            out.append(Handle(name, path, tag, False))
            seen.add(name)
    return out


def _parenthesized(text: str, start: int) -> str:
    """The balanced '( ... )' that opens at text[start]."""
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "("
        depth -= text[i] == ")"
        if depth == 0:
            return text[start : i + 1]
    return text[start:]


def calls_in(text: str, handles: set[str]) -> list[tuple[str, str]]:
    """(name, prototype on one line) for each public call the header
    'text' declares: its prototypes; a template's prototypes once per
    handle its type macro stands for (``DKS_TYPE`` as ``mds`` and as
    ``mdsc``, under the real names); and each function pointer in it
    (a class's call table or a callback), named ``(*member)``."""
    api = api_text(text)
    macros, values = template_types(text)
    aliases = {macro: [v for v in vs if v in handles] for macro, vs in values.items()}
    aliases = {macro: vs for macro, vs in aliases.items() if vs}
    calls: list[tuple[str, str]] = []
    for func, proto in declared_prototypes(api, templates=bool(aliases)):
        used = [a for a in aliases if re.search(rf"\b{a}\b", proto)]
        if not used:
            calls.append((func, proto))
            continue
        alias = used[0]
        for value in aliases[alias]:
            real = func
            if func in macros and macros[func][0] == alias:
                real = value + macros[func][1]
            expanded = re.sub(rf"\b{alias}\b", value, proto)
            calls.append((real, re.sub(rf"\b{func}\b", real, expanded)))
    code = " ".join(code_text(api).split())
    for m in _FUNCTION_POINTER.finditer(code):
        params = _parenthesized(code, m.end() - 1)
        calls.append((f"(*{m.group(1)})", f"{m.group(1)}{params}"))
    return calls


def classify(handles: list[Handle], headers: dict[str, str]) -> None:
    """Fill each handle's moving and returning calls from every public
    call."""
    by_name = {h.name: h for h in handles}
    if not by_name:
        return
    names = "|".join(re.escape(n) for n in by_name)
    # 'T **p' or 'T **const p'; a const pointee ('const T **', 'T *const *')
    # is an array read, not a handle written back
    double = re.compile(rf"(?<!const )\b({names})\s*\*\s*\*\s*(?:const\s+)?(\w+)")
    for text in headers.values():
        for func, proto in calls_in(text, set(by_name)):
            # the parameters only: 'T **f(...)' returns an array of T
            opened = re.search(rf"{re.escape(func)}\s*\(", proto)
            params = proto[opened.end() - 1 :] if opened else proto
            for t, param in double.findall(params):
                entry = f"{func}({param})"
                if entry not in by_name[t].moving:
                    by_name[t].moving.append(entry)
            ret = re.match(
                rf"^(?:[\w\[\]]+\s+)*?\b({names})\s*(\*?)\s*{re.escape(func)}\s*\(",
                proto,
            )
            if ret:
                h = by_name[ret.group(1)]
                takes = re.search(
                    rf"\(\s*(?:const\s+)?{re.escape(h.name)}\s*\*?\s*\w*\s*[,)]",
                    proto,
                )
                pointer = ret.group(2) == "*" or not h.opaque
                if takes and pointer:
                    h.returning.append(func)


def collect(repo: Path, wanted: tuple[str, ...] = ()) -> list[Handle]:
    headers = {
        p: t for p, t in collect_headers(repo).items() if not p.startswith(VENDORED)
    }
    handles: list[Handle] = []
    for path, text in headers.items():
        if wanted and path not in wanted:
            continue
        handles.extend(handles_in(path, text))
    classify(handles, headers)
    return sorted(handles, key=lambda h: (h.header, h.name))


def render(handles: list[Handle], listing: bool) -> tuple[str, int]:
    lines: list[str] = []
    problems = [p for h in handles for p in h.problems()]
    if listing:
        for h in handles:
            lines.append(
                f"{h.header}: {h.name} [{h.tag or 'untagged'}] "
                f"** {len(h.moving)}: {', '.join(h.moving[:6])}"
                + (" ..." if len(h.moving) > 6 else "")
            )
    lines += problems
    tagged = sum(1 for h in handles if h.tag in TAGS)
    lines.append(
        f"handles: {len(handles)} handles, {tagged} tagged, {len(problems)} to fix"
    )
    return "\n".join(lines), 1 if problems else 0


def run(repo: Path, wanted: tuple[str, ...], listing: bool) -> int:
    public = collect_headers(repo)
    unknown = [h for h in wanted if h not in public]
    if unknown:
        print(f"handles: not a public header: {', '.join(unknown)}")
        return 2
    text, code = render(collect(repo, wanted), listing)
    print(text)
    return code

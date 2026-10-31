"""``dktools modelcover``: public calls a container's model test never makes.

``dktools modelcover [HEADER ...]``

A model test drives a container through random sequences of its calls
and checks every read against a plain model.  A configuration call that
only ever runs on an empty structure looks covered by lines but has never
met the state that breaks it, so each container's model test should make
every public call.  The functions of a model test carry a marker comment
right before them:

    /* @model: src/multidict.h */
    static int multidictTestCaseDifferential_(...) { ... }

and the calls their bodies make count for that header (each helper the
test runs is marked too), a ``cleaner(fn)`` or ``cleanerName(fn, ...)``
scope counting as a call of ``fn``.  Reported: each public call of a modeled header
that no marked function makes, unless tools/modelcover-allow.txt has a
``HEADER CALL REASON`` line for it; a line that no longer matches fails
too.  The summary counts the headers that have no model test yet.

Files default to the tracked ``src/**/*.c`` and ``*.h``.  Exits 0 when
every gap is allowed and every allow line is used, 1 otherwise, 2 for a
malformed allow line.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .clex import code_mask
from .coverage import collect_headers
from .dupfuncs import function_bodies
from .gitutil import ToolError
from .handles import calls_in
from .retoken import VENDORED, sources

ALLOW_FILE = Path("tools/modelcover-allow.txt")
_MARK = re.compile(r"@model:\s*(\S+)\s*\*/")
_CALL = re.compile(rb"\b([A-Za-z_]\w*)\s*\(")
# A scope-exit cleaner names the function it calls: cleaner(fn), and
# cleanerName(fn, Name)
_CLEANER = re.compile(rb"\bcleaner(?:Name)?\s*\(\s*([A-Za-z_]\w*)")


@dataclass
class Model:
    header: str
    functions: list[str] = field(default_factory=list)
    called: set[str] = field(default_factory=set)


def _code(src: bytes) -> bytes:
    """'src' with comments and literals blanked, so only code matches."""
    mask = code_mask(src, directives_are_code=True)
    return bytes(c if mask[i] or c == 0x0A else 0x20 for i, c in enumerate(src))


def models(files: dict[str, str]) -> dict[str, Model]:
    """The model tests in 'files' (path -> source), by the header each
    marker names: the marked functions and every call their bodies make."""
    found: dict[str, Model] = {}
    for text in files.values():
        if "@model:" not in text:
            continue
        src = text.encode()
        code = _code(src)
        bodies = function_bodies(src)
        for m in _MARK.finditer(text):
            at = len(text[: m.end()].encode())
            following = [b for b in bodies if b[0] > at]
            if not following:
                continue
            _, name, start, stop = min(following, key=lambda b: b[0])
            model = found.setdefault(m.group(1), Model(m.group(1)))
            model.functions.append(name.decode())
            model.called |= {
                c.group(1).decode() for c in _CALL.finditer(code, start, stop)
            }
            model.called |= {
                c.group(1).decode() for c in _CLEANER.finditer(code, start, stop)
            }
    return found


def public_calls(header_text: str) -> set[str]:
    """The calls a header declares (not its function-pointer members or
    its test entry points)."""
    return {
        name
        for name, _ in calls_in(header_text, set())
        if not name.startswith("(*") and not name.endswith("Test")
    }


def missing(headers: dict[str, str], found: dict[str, Model]) -> dict[str, list[str]]:
    """Per modeled header, its public calls no marked function makes."""
    gaps: dict[str, list[str]] = {}
    for header, model in sorted(found.items()):
        text = headers.get(header)
        if text is None:
            raise ToolError(f"@model: {header}: no such public header")
        gaps[header] = sorted(public_calls(text) - model.called)
    return gaps


def parse_allow(text: str) -> dict[tuple[str, str], str]:
    allowed: dict[tuple[str, str], str] = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3:
            raise ToolError(f"{ALLOW_FILE}:{n}: HEADER CALL REASON needs a reason")
        allowed[(parts[0], parts[1])] = parts[2]
    return allowed


def _sources(repo: Path) -> list[str]:
    if (repo / ".git").exists():
        return sources(repo)
    found = [
        p.relative_to(repo).as_posix()
        for pattern in ("*.c", "*.h")
        for p in (repo / "src").rglob(pattern)
        if p.is_file()
    ]
    return sorted(p for p in found if not p.startswith(VENDORED))


def run(repo: Path, wanted: tuple[str, ...]) -> int:
    allow_path = repo / ALLOW_FILE
    allowed = parse_allow(
        allow_path.read_text(encoding="utf-8") if allow_path.exists() else ""
    )
    files = {
        p: (repo / p).read_text(encoding="utf-8", errors="replace")
        for p in _sources(repo)
    }
    headers = {
        p: t for p, t in collect_headers(repo).items() if not p.startswith(VENDORED)
    }
    found = models(files)
    if wanted:
        found = {h: m for h, m in found.items() if h in wanted}
    gaps = missing(headers, found)

    bad = [(h, c) for h, calls in gaps.items() for c in calls if (h, c) not in allowed]
    for header, call in bad:
        print(f"{header}: {call}: no model test makes it")
    used = {(h, c) for h, calls in gaps.items() for c in calls}
    stale = [k for k in allowed if k[0] in gaps and k not in used]
    for header, call in stale:
        print(f"{ALLOW_FILE}: {header} {call}: allowed but a model test makes it")

    unmodeled = sum(1 for h in headers if h not in found and public_calls(headers[h]))
    print(
        f"modelcover: {len(bad)} calls to cover, {len(used) - len(bad)} allowed, "
        f"{len(stale)} stale allow lines; {len(found)} headers modeled, "
        f"{unmodeled} with no model test"
    )
    return 1 if bad or stale else 0

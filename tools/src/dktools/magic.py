"""``dktools magic``: numbers that should have a name.

``dktools magic [FILE ...]``

Two rules, each a place where a bare number hides a fact the code
depends on:

- ``repeated-limit``: one integer (10 or more) used as a limit in two or
  more checks: an if that refuses (its statement ends in a return,
  perhaps after freeing what it holds) or clamps (sets the value it
  compared against).  A limit belongs to one module; another check that
  writes the number again drifts when the owner changes it.  Use a named
  constant from the module the rule belongs to, or the standard's name
  for a fact of the language (UINT64_WIDTH).
- ``positional-fields``: inside a snapshot, restore or persist function,
  one array read at three or more literal positions (``v[3]``,
  ``v[7]``).  The writer and the reader of a format must agree on its
  field order; name the positions with one enum per format, used by
  both.

Only code counts: comments, literals and ``DATAKIT_TEST`` sections never
match.  An exception is a line ``PATH RULE KEY REASON`` in
tools/magic-allow.txt (KEY: the number for repeated-limit,
``function:array`` for positional-fields, ``*`` for every hit of the rule
in that file); the reason is required, and a
line that no longer matches anything fails too, so the list stays true.

Files default to the tracked ``src/**/*.c`` and ``*.h``.  Exits 0 when
every hit is allowed and every allow line is used, 1 otherwise, 2 for a
missing file or a malformed allow line.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .clex import code_mask
from .dupfuncs import function_bodies
from .gitutil import ToolError
from .retoken import VENDORED, sources

ALLOW_FILE = Path("tools/magic-allow.txt")
REPEATED_LIMIT = "repeated-limit"
POSITIONAL = "positional-fields"
_KNOWN = frozenset({REPEATED_LIMIT, POSITIONAL})
_USE = {
    REPEATED_LIMIT: "a named constant from the module the limit belongs to",
    POSITIONAL: "an enum naming the format's fields, shared by writer and reader",
}

# A limit worth naming: small numbers are lengths and counts, not rules
LIMIT_MIN = 10
# Positions of one array before they read as a record's fields
POSITIONS_MIN = 3

_IF = re.compile(rb"(?<![\w#])if\s*\(")
_LITERAL = rb"(?P<lit>0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*"
_OP = rb"(?<![<>\-])(?:<=|>=|<|>)(?![<>])"
_RIGHT = re.compile(_OP + rb"\s*" + _LITERAL + rb"(?![\w.])")
_LEFT = re.compile(rb"(?<![\w.])" + _LITERAL + rb"\s*" + _OP)
_PERSIST_NAME = re.compile(rb"(?i)restore|snapshot|persist")
_INDEX = re.compile(rb"(?<![\w.])(?P<arr>[A-Za-z_]\w*)\s*\[\s*(?P<idx>[0-9]+)\s*\]")
_PREV_WORD = re.compile(rb"([A-Za-z_]\w*)\s*\**\s*$")
_NOT_TYPES = frozenset(
    {b"return", b"case", b"sizeof", b"else", b"do", b"goto", b"alignof"}
)
_TEST_OPEN = re.compile(
    rb"#\s*if(?:def\s+DATAKIT_TEST\b|\s+defined\s*\(?\s*DATAKIT_TEST\b)"
)
_DIRECTIVE = re.compile(rb"^\s*#\s*(if|ifdef|ifndef|endif)\b", re.MULTILINE)


@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    rule: str
    key: str


def _code_text(src: bytes) -> bytes:
    """'src' with comments, literals and DATAKIT_TEST sections blanked
    (newlines kept)."""
    mask = code_mask(src, directives_are_code=True)
    out = bytearray(c if mask[i] or c == 0x0A else 0x20 for i, c in enumerate(src))
    depth = 0
    start = -1
    for m in _DIRECTIVE.finditer(out):
        kind = m.group(1)
        if kind in (b"if", b"ifdef", b"ifndef"):
            if depth:
                depth += 1
            elif _TEST_OPEN.match(out, m.start() + m.group(0).index(b"#")):
                depth, start = 1, m.start()
        elif kind == b"endif" and depth:
            depth -= 1
            if not depth:
                for i in range(start, m.end()):
                    if out[i] != 0x0A:
                        out[i] = 0x20
    return bytes(out)


def _close(code: bytes, open_: int) -> int:
    """The index of the ')' closing the '(' at 'open_', or -1."""
    depth = 0
    for i in range(open_, len(code)):
        if code[i] == 0x28:
            depth += 1
        elif code[i] == 0x29:
            depth -= 1
            if not depth:
                return i
    return -1


def _value(lit: bytes) -> int:
    return int(lit, 16) if lit[:2] in (b"0x", b"0X") else int(lit, 10)


def _body(code: bytes, close: int) -> bytes:
    """The statement an if's condition ending at 'close' guards: a braced
    block's inside, or the one statement up to its ';'."""
    m = re.compile(rb"\s*").match(code, close + 1)
    start = m.end() if m else close + 1
    if start < len(code) and code[start] == 0x7B:
        depth = 0
        for i in range(start, len(code)):
            if code[i] == 0x7B:
                depth += 1
            elif code[i] == 0x7D:
                depth -= 1
                if not depth:
                    return code[start + 1 : i]
        return b""
    stop = code.find(b";", start)
    return code[start : stop + 1] if stop >= 0 else b""


def _refuses(body: bytes) -> bool:
    """Whether a guarded statement ends by returning (a refusal, perhaps
    after freeing what it holds)."""
    statements = [s.strip() for s in body.split(b";") if s.strip()]
    return bool(statements) and re.match(rb"\}?\s*return\b", statements[-1]) is not None


def _limits(code: bytes) -> list[tuple[int, int]]:
    """(line, value) of each literal limit in a refusal check or a clamp
    (an if that sets the value it compared against)."""
    found: set[tuple[int, int]] = set()
    for m in _IF.finditer(code):
        open_ = m.end() - 1
        close = _close(code, open_)
        if close < 0:
            continue
        body = _body(code, close)
        refusal = _refuses(body)
        for pattern in (_RIGHT, _LEFT):
            for c in pattern.finditer(code, open_, close):
                lit = c.group("lit")
                value = _value(lit)
                clamp = re.search(
                    rb"(?<![=!<>])=\s*" + re.escape(lit) + rb"[uUlL]*(?![\w.])", body
                )
                if value >= LIMIT_MIN and (refusal or clamp):
                    found.add((code.count(b"\n", 0, c.start()) + 1, value))
    return sorted(found)


def _positional(path: str, src: bytes, code: bytes) -> list[Hit]:
    """One hit per (persist function, array) read at POSITIONS_MIN or
    more literal positions."""
    hits: list[Hit] = []
    for _, name, start, stop in function_bodies(src):
        if not _PERSIST_NAME.search(name):
            continue
        seen: dict[bytes, tuple[int, set[int]]] = {}
        for m in _INDEX.finditer(code, start, stop):
            prev = _PREV_WORD.search(code, max(start, m.start() - 64), m.start())
            if prev and prev.group(1) not in _NOT_TYPES:
                continue  # a declaration: 'uint64_t v[8]', 'databox *row[2]'
            first, positions = seen.setdefault(m.group("arr"), (m.start(), set()))
            positions.add(int(m.group("idx")))
        for arr, (first, positions) in seen.items():
            if len(positions) >= POSITIONS_MIN:
                line = code.count(b"\n", 0, first) + 1
                hits.append(
                    Hit(path, line, POSITIONAL, f"{name.decode()}:{arr.decode()}")
                )
    return hits


def scan_files(files: dict[str, bytes]) -> list[Hit]:
    """Every rule's hits across 'files' (path -> source)."""
    limits: dict[str, list[tuple[int, int]]] = {}
    hits: list[Hit] = []
    for path, src in files.items():
        code = _code_text(src)
        limits[path] = _limits(code)
        hits += _positional(path, src, code)
    checks: dict[int, int] = {}
    for found in limits.values():
        for _, value in found:
            checks[value] = checks.get(value, 0) + 1
    for path, found in limits.items():
        for line, value in found:
            if checks[value] > 1:
                hits.append(Hit(path, line, REPEATED_LIMIT, str(value)))
    return sorted(hits, key=lambda h: (h.path, h.line, h.rule, h.key))


def parse_allow(text: str) -> dict[tuple[str, str, str], str]:
    """The allow file's (path, rule, key) -> reason entries; ToolError for
    a line without a reason or naming no rule."""
    allowed: dict[tuple[str, str, str], str] = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 3)
        if len(parts) < 4:
            raise ToolError(f"{ALLOW_FILE}:{n}: PATH RULE KEY REASON needs a reason")
        path, rule, key, reason = parts
        if rule not in _KNOWN:
            raise ToolError(f"{ALLOW_FILE}:{n}: no rule named {rule!r}")
        allowed[(path, rule, key)] = reason
    return allowed


def _default_sources(repo: Path) -> list[str]:
    """The tracked src C files in a git checkout; every src C file in an
    exported tree (a gate's copy, which is not a checkout)."""
    if (repo / ".git").exists():
        return sources(repo)
    found = [
        p.relative_to(repo).as_posix()
        for pattern in ("*.c", "*.h")
        for p in (repo / "src").rglob(pattern)
        if p.is_file()
    ]
    return sorted(p for p in found if not p.startswith(VENDORED))


def run(repo: Path, paths: tuple[str, ...]) -> int:
    allow_path = repo / ALLOW_FILE
    allowed = parse_allow(
        allow_path.read_text(encoding="utf-8") if allow_path.exists() else ""
    )
    # A limit repeats across files, so the whole tree is read even when
    # some files are named; the named ones are the ones reported
    tree = _default_sources(repo)
    named = list(paths) or tree
    if not named:
        raise ToolError(f"{repo}: no C sources found under src/")
    files: dict[str, bytes] = {}
    for path in dict.fromkeys(tree + named):
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        files[path] = target.read_bytes()
    report = set(named)
    hits = [h for h in scan_files(files) if h.path in report]

    def allows(h: Hit) -> tuple[str, str, str] | None:
        for key in (h.key, "*"):
            if (h.path, h.rule, key) in allowed:
                return (h.path, h.rule, key)
        return None

    bad = [h for h in hits if allows(h) is None]
    for h in bad:
        print(f"{h.path}:{h.line}: {h.rule} {h.key}: use {_USE[h.rule]}")

    used = {allows(h) for h in hits}
    stale = [k for k in allowed if k[0] in report and k not in used]
    for path, rule, key in stale:
        print(f"{ALLOW_FILE}: {path} {rule} {key}: allowed but no longer found")

    print(
        f"magic: {len(bad)} hits to fix, {len(hits) - len(bad)} allowed, "
        f"{len(stale)} stale allow lines"
    )
    return 1 if bad or stale else 0

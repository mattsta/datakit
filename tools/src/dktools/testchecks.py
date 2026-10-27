"""``dktools testchecks``: TEST blocks that check nothing.

``dktools testchecks [FILE ...]``

A ``TEST(name) { ... }`` or ``TEST_DESC(fmt, ...) { ... }`` block passes
when nothing in it reports a failure, so a block with no check in it
passes whatever the code under test does.  This lists every block whose
code names no check:

- a ctest.h check (``ERR``, ``ERROR``, ``TEST_BYTES``, ``TEST_HEX``),
  ``assert``, or the test's failure count ``err`` (``err += f()``);
- a macro, defined in a scanned file or in any header under src,
  whose body names one of those, directly or through another such
  macro.

Blocks that measure rather than check are not listed: those named for
it (a name with the word perf, performance, speed, bench, benchmark,
throughput, timing, latency, comparison, analysis, workload or
head-to-head, any case) and those that report a perf.h timing
(``TIME_FINISH``).

Only code counts: comments and literals never match, so a check that is
only mentioned in a comment does not count.

A block whose whole contract is that it runs (a printer, a free of
NULL) is an exception: a line ``PATH :: NAME :: REASON`` in
tools/testchecks-allow.txt, NAME as listed.  The reason is required,
and a line naming no block that is listed any more fails too, so the
list stays true.

Files default to the tracked ``src/**/*.c`` and ``*.h``.  Exits 0 when
every block listed is allowed and every allow line is used, 1
otherwise, 2 for a missing file or a malformed allow line.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .gitutil import ToolError
from .idioms import _code_text, _default_sources

ALLOW_FILE = Path("tools/testchecks-allow.txt")

#: Names whose appearance in a block is a check
CHECKS = frozenset({b"ERR", b"ERROR", b"TEST_BYTES", b"TEST_HEX", b"assert", b"err"})

#: perf.h's timing macros: a block that reports a timing measures
TIMERS = frozenset({b"TIME_FINISH", b"PERF_TIMERS_FINISH_PRINT_RESULTS"})

_TEST = re.compile(rb"\bTEST(?:_DESC)?\s*\(")
_DEFINE = re.compile(rb"^[ \t]*#[ \t]*define[ \t]+(\w+)", re.MULTILINE)
_IDENT = re.compile(rb"\b[A-Za-z_]\w*\b")
_MEASURES = re.compile(
    rb"\b(?:perf|performance|speed|bench|benchmark|throughput|timing|latency"
    rb"|comparison|analysis|workload|head-to-head)\b",
    re.IGNORECASE,
)
_SEP = " :: "


@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    name: str


def _match(code: bytes, i: int, open_: int, close: int) -> int:
    """The index just past the bracket that closes the one at 'i'
    (len(code) when it never closes)."""
    depth = 0
    for j in range(i, len(code)):
        if code[j] == open_:
            depth += 1
        elif code[j] == close:
            depth -= 1
            if depth == 0:
                return j + 1
    return len(code)


def _macro_bodies(code: bytes) -> dict[bytes, bytes]:
    """Each macro name defined in 'code' and its body (continuation lines
    joined)."""
    bodies: dict[bytes, bytes] = {}
    for m in _DEFINE.finditer(code):
        end = m.end()
        while True:
            nl = code.find(b"\n", end)
            if nl < 0:
                end = len(code)
                break
            # A continuation: the newline follows a backslash
            if code[nl - 1 : nl] == b"\\":
                end = nl + 1
                continue
            end = nl
            break
        bodies[m.group(1)] = code[m.end() : end]
    return bodies


def check_names(*codes: bytes) -> frozenset[bytes]:
    """CHECKS plus every macro in 'codes' whose body names a check,
    directly or through another such macro."""
    bodies: dict[bytes, bytes] = {}
    for code in codes:
        bodies.update(_macro_bodies(code))
    names = set(CHECKS)
    grew = True
    while grew:
        grew = False
        for name, body in bodies.items():
            if name not in names and names.intersection(_IDENT.findall(body)):
                names.add(name)
                grew = True
    return frozenset(names)


def scan(
    path: str, src: bytes, ctest: bytes = b"", checks: frozenset[bytes] | None = None
) -> list[Hit]:
    """The TEST blocks in 'src' that name no check.  'checks' is the set
    of check names; by default, those of 'src' and 'ctest' (ctest.h's
    text)."""
    code = _code_text(src)
    if checks is None:
        checks = check_names(code, _code_text(ctest))
    hits: list[Hit] = []
    for m in _TEST.finditer(code):
        line_start = code.rfind(b"\n", 0, m.start()) + 1
        if code[line_start : m.start()].lstrip().startswith(b"#"):
            continue  # the macro's own definition
        args_end = _match(code, m.end() - 1, 0x28, 0x29)
        rest = code[args_end:]
        body_at = len(rest) - len(rest.lstrip())
        if rest[body_at : body_at + 1] != b"{":
            continue  # a mention, not a block
        name = b" ".join(src[m.end() : args_end - 1].split())
        if _MEASURES.search(name):
            continue
        open_ = args_end + body_at
        body = code[open_ : _match(code, open_, 0x7B, 0x7D)]
        names = set(_IDENT.findall(body))
        if checks.isdisjoint(names) and TIMERS.isdisjoint(names):
            text = name.decode("utf-8", "replace")
            hits.append(Hit(path, code.count(b"\n", 0, m.start()) + 1, text))
    return hits


def parse_allow(text: str) -> dict[tuple[str, str], str]:
    """The allow file's (path, name) -> reason entries; ToolError for a
    line without all three parts."""
    allowed: dict[tuple[str, str], str] = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(_SEP)]
        if len(parts) != 3 or not all(parts):
            raise ToolError(f"{ALLOW_FILE}:{n}: want PATH{_SEP}NAME{_SEP}REASON")
        path, name, reason = parts
        allowed[(path, name)] = reason
    return allowed


def run(repo: Path, paths: tuple[str, ...]) -> int:
    allow_path = repo / ALLOW_FILE
    allowed = parse_allow(
        allow_path.read_text(encoding="utf-8") if allow_path.exists() else ""
    )
    files = list(paths) or _default_sources(repo)
    if not files:
        raise ToolError(f"{repo}: no C sources found under src/")
    sources: dict[str, bytes] = {}
    for path in files:
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        sources[path] = target.read_bytes()

    # Check macros come from every header (a test's macros may live in
    # its module's header) as well as from the files scanned
    headers = [p for p in _default_sources(repo) if p.endswith(".h")]
    codes = [_code_text(src) for src in sources.values()]
    codes += [_code_text((repo / h).read_bytes()) for h in headers if h not in sources]
    checks = check_names(*codes)
    hits: list[Hit] = []
    for path, src in sources.items():
        hits += scan(path, src, checks=checks)

    bad = [h for h in hits if (h.path, h.name) not in allowed]
    for h in bad:
        print(f"{h.path}:{h.line}: TEST {h.name}: checks nothing")

    # An allow line is stale when its file was scanned and no block it
    # names was listed
    used = {(h.path, h.name) for h in hits}
    stale = [k for k in allowed if k[0] in sources and k not in used]
    for path, name in stale:
        print(f"{ALLOW_FILE}: {path}{_SEP}{name}: allowed but no longer listed")

    print(
        f"testchecks: {len(bad)} blocks with no check, {len(hits) - len(bad)} "
        f"allowed, {len(stale)} stale allow lines, in {len(files)} files"
    )
    return 1 if bad or stale else 0

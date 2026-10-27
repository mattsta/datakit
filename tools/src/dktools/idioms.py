"""``dktools idioms``: hand-written forms of what a shared helper does.

``dktools idioms [FILE ...]``

Each rule names one idiom the platform has a shared form for, and the
helper to use instead:

- ``byte-shift-loop``: ``x << (8 * i)`` / ``>> (i * 8)``, a byte at a
  time in a stated order -> dkEndian.h dkLoad*/dkStore*
- ``inline-lcg``: a multiply by 6364136223846793005 -> dkLcg64Next
- ``inline-xorshift64star``: a multiply by 0x2545F4914F6CDD1D
  -> dkXorshift64StarNext
- ``rand-64``: ``rand() << 32`` (two 31-bit draws, bits 31 and 63 never
  set) -> dkSplitmix64Next / dkRandom64
- ``qsort-comparator``: a static ``int f(const void *, const void *)``
  comparing two plain numbers -> dkSort.h
- ``memcmp-err``: ``if (memcmp(...) != 0) { ERR(...); }`` -> TEST_BYTES
  (``dktools testbytes`` rewrites it)
- ``encode-op-boxes``: ``flexNewFromDataboxPtrsExact(boxes, argc)`` in a
  persist encodeOp -> persistOpEncodeBoxes
- ``div-ceil``: ``(a + b - 1) / b`` (overflows near the top) -> dkDivCeil

Only code counts: comments and literals never match, preprocessor lines
do (a macro body is code).  Each rule skips the file that defines its
shared form.  An exception is a line ``PATH RULE REASON`` in
tools/idioms-allow.txt; the reason is required, and a line that no
longer matches anything fails too, so the list stays true.

Files default to the tracked ``src/**/*.c`` and ``*.h``.  Exits 0 when
every hit is allowed and every allow line is used, 1 otherwise, 2 for a
missing file or a malformed allow line.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from . import testbytes
from .clex import code_mask
from .gitutil import ToolError
from .retoken import VENDORED, sources

ALLOW_FILE = Path("tools/idioms-allow.txt")


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[bytes]
    use: str
    home: frozenset[str] = frozenset()


RULES: tuple[Rule, ...] = (
    Rule(
        "byte-shift-loop",
        re.compile(rb"(?:<<|>>)\s*\(\s*(?:8u?\s*\*\s*\w+|\w+\s*\*\s*8u?)\s*\)"),
        "dkEndian.h dkLoadLe*/dkStoreLe* (or Be*)",
        frozenset({"src/dkEndian.h"}),
    ),
    Rule(
        "inline-lcg",
        re.compile(rb"\*\s*6364136223846793005"),
        "dkRandom.h dkLcg64Next",
        frozenset({"src/dkRandom.h"}),
    ),
    Rule(
        "inline-xorshift64star",
        re.compile(rb"\*\s*(?:2685821657736338717|0x2545F4914F6CDD1D)", re.IGNORECASE),
        "dkRandom.h dkXorshift64StarNext",
        frozenset({"src/dkRandom.h"}),
    ),
    Rule(
        "rand-64",
        re.compile(rb"\brand\s*\(\s*\)\s*<<\s*32\b"),
        "dkRandom.h dkSplitmix64Next (replayable) or dkRandom64",
    ),
    Rule(
        "qsort-comparator",
        # Two loads of one plain type, then straight to the return: a
        # comparator that orders through what it loads is not this
        re.compile(
            rb"\bint\s+\w+\s*\(\s*const\s+void\s*\*\s*\w*\s*,\s*const\s+void\s*\*"
            rb"\s*\w*\s*\)\s*\{\s*(?:const\s+)?(u?int32_t|u?int64_t|double)\s+\w+"
            rb"\s*=\s*\*\s*\(\s*const\s+\1\s*\*\s*\)\s*\w+\s*;\s*"
            rb"(?:const\s+)?\1\s+\w+\s*=\s*\*\s*\(\s*const\s+\1\s*\*\s*\)\s*\w+\s*;"
            rb"\s*return\b"
        ),
        "dkSort.h dkSortArray / dkCompare*",
        frozenset({"src/dkSort.h"}),
    ),
    Rule(
        "encode-op-boxes",
        # an encodeOp turning its argc operand boxes into the op body
        re.compile(rb"\bflexNewFromDataboxPtrsExact\s*\(\s*\w+\s*,\s*argc\s*\)"),
        "persist.h persistOpEncodeBoxes",
        frozenset({"src/persist.c"}),
    ),
    Rule(
        "div-ceil",
        re.compile(rb"\+\s*\(?\s*(\w+)\s*\)?\s*-\s*1\s*\)\s*/\s*\(?\s*\1\b"),
        "dkBits.h dkDivCeil",
        frozenset({"src/dkBits.h"}),
    ),
)

MEMCMP_ERR = "memcmp-err"
_KNOWN = {r.name for r in RULES} | {MEMCMP_ERR}


@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    rule: str


def _code_text(src: bytes) -> bytes:
    """'src' with comments and literals blanked (newlines kept), so a
    pattern sees only code; preprocessor lines count as code."""
    mask = code_mask(src, directives_are_code=True)
    return bytes(c if mask[i] or c == 0x0A else 0x20 for i, c in enumerate(src))


def scan(path: str, src: bytes) -> list[Hit]:
    """Every rule's matches in 'src' (one per line and rule)."""
    code = _code_text(src)
    hits: set[Hit] = set()
    for rule in RULES:
        if path in rule.home:
            continue
        for m in rule.pattern.finditer(code):
            hits.add(Hit(path, code.count(b"\n", 0, m.start()) + 1, rule.name))
    _, count = testbytes.rewrite(src)
    if count:
        for m in re.finditer(rb"\bif\s*\(\s*memcmp\s*\(", code):
            hits.add(Hit(path, code.count(b"\n", 0, m.start()) + 1, MEMCMP_ERR))
    return sorted(hits, key=lambda h: (h.path, h.line, h.rule))


def parse_allow(text: str) -> dict[tuple[str, str], str]:
    """The allow file's (path, rule) -> reason entries; ToolError for a
    line without a reason or naming no rule."""
    allowed: dict[tuple[str, str], str] = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3:
            raise ToolError(f"{ALLOW_FILE}:{n}: PATH RULE REASON needs a reason")
        path, rule, reason = parts
        if rule not in _KNOWN:
            raise ToolError(f"{ALLOW_FILE}:{n}: no rule named {rule!r}")
        allowed[(path, rule)] = reason
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
    files = list(paths) or _default_sources(repo)
    if not files:
        # A gate that read nothing would pass without checking anything
        raise ToolError(f"{repo}: no C sources found under src/")
    hits: list[Hit] = []
    for path in files:
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        hits += scan(path, target.read_bytes())

    use = {r.name: r.use for r in RULES}
    use[MEMCMP_ERR] = "ctest.h TEST_BYTES (dktools testbytes rewrites it)"
    bad = [h for h in hits if (h.path, h.rule) not in allowed]
    for h in bad:
        print(f"{h.path}:{h.line}: {h.rule}: use {use[h.rule]}")

    # An allow line is stale when its file was scanned and nothing it names
    # matched there any more
    scanned = set(files)
    used = {(h.path, h.rule) for h in hits}
    stale = [k for k in allowed if k[0] in scanned and k not in used]
    for path, rule in stale:
        print(f"{ALLOW_FILE}: {path} {rule}: allowed but no longer found")

    print(
        f"idioms: {len(bad)} hits to fix, {len(hits) - len(bad)} allowed, "
        f"{len(stale)} stale allow lines"
    )
    return 1 if bad or stale else 0

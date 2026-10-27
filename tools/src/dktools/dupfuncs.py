"""``dktools dupfuncs``: function bodies written out more than once.

``dktools dupfuncs [--min-tokens N] [FILE ...]``

Finds each function definition at file scope (a ``{`` at brace depth 0
right after the ``)`` that closes a parameter list) and compares the
bodies as tokens: comments and layout do not count, literals do.  Every
body of at least ``--min-tokens`` tokens (default 40) that appears at
two or more definitions is listed with each place it is defined, the
largest first: each group is a candidate for one shared helper.

Preprocessor lines are not code here, so a function a macro defines is
not seen.  Files default to the tracked ``src/**/*.c`` and ``*.h``.
Exits 0 (the report is advice), 2 for a missing file.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .clex import code_mask
from .gitutil import ToolError
from .retoken import sources

_TOKEN = re.compile(
    rb"""[A-Za-z_]\w*                       # identifier or keyword
       | \.?\d(?:[\w.']|[eEpP][+-])*         # number
       | "(?:\\.|[^"\\\n])*"                 # string literal
       | '(?:\\.|[^'\\\n])*'                 # character literal
       | <<=|>>=|\.\.\.|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=
       | \S                                  # any other punctuator
    """,
    re.VERBOSE,
)
_NAME_BEFORE = re.compile(rb"([A-Za-z_]\w*)\s*$")
_NOT_FUNCTIONS = frozenset(
    {b"if", b"for", b"while", b"switch", b"return", b"sizeof", b"alignof"}
)


@dataclass(frozen=True)
class Definition:
    path: str
    line: int
    name: str


def _matching_open(mask: bytearray, src: bytes, close: int) -> int | None:
    """The index of the '(' that the ')' at 'close' closes."""
    depth = 0
    i = close
    while i >= 0:
        if mask[i]:
            if src[i] == 0x29:
                depth += 1
            elif src[i] == 0x28:
                depth -= 1
                if depth == 0:
                    return i
        i -= 1
    return None


def _without_comments(src: bytes, start: int, stop: int) -> bytes:
    """src[start:stop] with each comment as a space; literals kept."""
    keep = code_mask(src[start:stop], directives_are_code=True)
    lit_or_code = code_mask(
        src[start:stop], directives_are_code=True, comments_are_code=True
    )
    out = bytearray(src[start:stop])
    for i in range(len(out)):
        if lit_or_code[i] and not keep[i]:
            out[i] = 0x20
    return bytes(out)


def definitions(src: bytes) -> list[tuple[int, bytes, tuple[bytes, ...]]]:
    """Each file-scope function definition in 'src': the offset of its
    name, the name, and its body's tokens (braces included)."""
    mask = code_mask(src)
    found: list[tuple[int, bytes, tuple[bytes, ...]]] = []
    depth = 0
    body_start = -1
    head: tuple[int, bytes] | None = None
    for i, c in enumerate(src):
        if not mask[i]:
            continue
        if c == 0x7B:  # {
            if depth == 0:
                head = _head(src, mask, i)
                body_start = i
            depth += 1
        elif c == 0x7D:  # }
            depth -= 1
            if depth == 0 and head is not None:
                text = _without_comments(src, body_start, i + 1)
                tokens = tuple(m.group() for m in _TOKEN.finditer(text))
                found.append((head[0], head[1], tokens))
                head = None
            depth = max(depth, 0)
    return found


def _head(src: bytes, mask: bytearray, brace: int) -> tuple[int, bytes] | None:
    """The function name's offset and the name, when the '{' at 'brace'
    opens a function body; None for any other brace."""
    j = brace - 1
    while j >= 0 and (not mask[j] or src[j] in b" \t\r\n"):
        j -= 1
    if j < 0 or src[j] != 0x29:
        return None
    open_paren = _matching_open(mask, src, j)
    if open_paren is None:
        return None
    m = _NAME_BEFORE.search(src, max(0, open_paren - 128), open_paren)
    if m is None or m.group(1) in _NOT_FUNCTIONS:
        return None
    return m.start(1), m.group(1)


def duplicates(
    files: dict[str, bytes], min_tokens: int
) -> list[tuple[int, list[Definition]]]:
    """Groups of two or more definitions with the same body of at least
    'min_tokens' tokens, largest body first: (token count, places)."""
    groups: dict[tuple[bytes, ...], list[Definition]] = {}
    for path, src in files.items():
        for offset, name, tokens in definitions(src):
            if len(tokens) < min_tokens:
                continue
            line = src.count(b"\n", 0, offset) + 1
            groups.setdefault(tokens, []).append(Definition(path, line, name.decode()))
    found = [(len(t), places) for t, places in groups.items() if len(places) > 1]
    found.sort(key=lambda g: (-g[0], g[1][0].path, g[1][0].line))
    return found


def run(repo: Path, paths: tuple[str, ...], min_tokens: int) -> int:
    files: dict[str, bytes] = {}
    for path in list(paths) or sources(repo):
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        files[path] = target.read_bytes()
    groups = duplicates(files, min_tokens)
    for count, places in groups:
        print(f"{count} tokens, {len(places)} copies:")
        for d in places:
            print(f"  {d.path}:{d.line}: {d.name}")
    print(
        f"dupfuncs: {len(groups)} bodies of {min_tokens}+ tokens defined twice or more"
    )
    return 0

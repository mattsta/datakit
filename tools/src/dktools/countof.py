"""``dktools countof``: element counts written by hand become countof(X).

Rewrites, where every byte is code (macro bodies included; comments and
literals never), each same-operand element count of an array:

- ``sizeof(X) / sizeof(*X)`` and ``sizeof(X) / sizeof(X[0])`` (also
  ``sizeof((X)[0])`` and the unparenthesized ``sizeof X / sizeof X[0]``)
  become ``countof(X)``, X being a name with any ``.member``, ``->member``
  and ``[index]`` after it;
- ``COUNT_ARRAY(X)`` becomes ``countof(X)``.

A ratio with any other divisor (``sizeof(buf) / sizeof(uint8_t)``, another
operand, another index) stays. countof refuses a pointer at compile time,
so a site that stops compiling was counting a pointer's bytes.

Prints each file's count; ``--dry-run`` changes nothing. Exits 0, or 2 for
a missing file.
"""

from __future__ import annotations

import re
from pathlib import Path

from .clex import code_mask
from .gitutil import ToolError
from .retoken import retoken, sources

#: an operand: a name, then members and indexes
OPERAND = rb"[A-Za-z_]\w*(?:\s*(?:\.|->)\s*[A-Za-z_]\w*|\s*\[[^\[\]]*\])*"
RATIO = re.compile(
    rb"\bsizeof\s*(?:\(\s*(?P<a>" + OPERAND + rb")\s*\)|(?P<b>" + OPERAND + rb"))"
    rb"\s*/\s*"
    rb"sizeof\s*(?:\(\s*(?P<d>[^()]*(?:\([^()]*\)[^()]*)*)\s*\)|(?P<e>"
    + OPERAND
    + rb"))"
)
SPACE = re.compile(rb"\s+")


def _same_element(operand: bytes, divisor: bytes) -> bool:
    """Whether 'divisor' is the first element of 'operand': *X, X[0] or
    (X)[0], spacing aside."""
    x = SPACE.sub(b"", operand)
    d = SPACE.sub(b"", divisor)
    return d in (b"*" + x, x + b"[0]", b"(" + x + b")[0]", b"*(" + x + b")")


#: the definitions of countof and COUNT_ARRAY themselves, never rewritten
#: (either would become a macro defined as itself)
DEFINITION = re.compile(
    rb"(?m)^[ \t]*#[ \t]*(?:define|ifndef|ifdef)[ \t]+(?:countof|COUNT_ARRAY)\b.*$"
)


def rewrite(src: bytes) -> tuple[bytes, int]:
    """'src' with each element count made countof(X), and how many."""
    kept = [(m.start(), m.end()) for m in DEFINITION.finditer(src)]
    out_parts = []
    renamed = 0
    last = 0
    for start, end in kept:
        part, n = retoken(src[last:start], b"COUNT_ARRAY", b"countof")
        out_parts += [part, src[start:end]]
        renamed += n
        last = end
    part, n = retoken(src[last:], b"COUNT_ARRAY", b"countof")
    out_parts.append(part)
    src = b"".join(out_parts)
    renamed += n
    kept = [(m.start(), m.end()) for m in DEFINITION.finditer(src)]
    mask = code_mask(src, directives_are_code=True)
    for start, end in kept:
        mask[start:end] = bytes(end - start)  # not rewritten
    out = bytearray()
    last = 0
    count = renamed
    for m in RATIO.finditer(src):
        operand = m.group("a") or m.group("b")
        divisor = m.group("d") if m.group("d") is not None else m.group("e")
        if not all(mask[m.start() : m.end()]) or not _same_element(operand, divisor):
            continue
        out += src[last : m.start()]
        out += b"countof(" + SPACE.sub(b"", operand) + b")"
        last = m.end()
        count += 1
    out += src[last:]
    return bytes(out), count


def run(repo: Path, paths: tuple[str, ...], dry_run: bool) -> int:
    targets = list(paths) or sources(repo)
    total = 0
    for path in targets:
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        src = target.read_bytes()
        out, count = rewrite(src)
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would rewrite" if dry_run else "rewrote"
    print(f"countof: {verb} {total} element count(s)")
    return 0

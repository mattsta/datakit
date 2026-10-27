"""``dktools testbytes``: byte comparisons in tests as TEST_BYTES checks.

Rewrites each test check of exactly the form::

    if (memcmp(GOT, WANT, N) != 0) {
        ERR(FORMAT, ARGS...);
    }

as ``TEST_BYTES(err, GOT, WANT, N, FORMAT, ARGS...);`` (ctest.h), which
counts the failure into ``err`` as ERR does and also prints both sides in
hex.  Only that shape is rewritten: an ``if`` with another condition,
more than the one ERR in its body, or an ``else`` is left as written, as
is any of it inside a comment, a string or a preprocessor line.

Files default to the tracked ``src/**/*.c`` and ``*.h``.  Prints each
file's count; exits 0, 2 for a missing file.
"""

from __future__ import annotations

import re
from pathlib import Path

from .clex import code_mask
from .dropargs import split_args
from .gitutil import ToolError
from .retoken import sources

_IF_MEMCMP = re.compile(rb"\bif\s*\(\s*memcmp\s*\(")
_NOT_EQUAL_ZERO = re.compile(rb"\s*!=\s*0\s*\)\s*\{\s*ERR\s*\(")
_CLOSE = re.compile(rb"\s*;\s*\}")
_ELSE = re.compile(rb"\s*else\b")


def _all_code(mask: bytearray, start: int, stop: int) -> bool:
    return all(mask[start:stop])


def rewrite(src: bytes) -> tuple[bytes, int]:
    """'src' with every single-ERR memcmp check as TEST_BYTES, and how
    many were rewritten."""
    mask = code_mask(src)
    edits: list[tuple[int, int, bytes]] = []
    for m in _IF_MEMCMP.finditer(src):
        start = m.start()
        if not _all_code(mask, start, m.end()):
            continue
        open_cmp = m.end() - 1
        found = split_args(src, mask, open_cmp)
        if found is None or len(found[0]) != 3:
            continue
        cmp_args, cmp_close = found
        tail = _NOT_EQUAL_ZERO.match(src, cmp_close + 1)
        if tail is None or not _all_code(mask, tail.start(), tail.end()):
            continue
        open_err = tail.end() - 1
        found = split_args(src, mask, open_err)
        if found is None or not found[0][0].strip():
            continue
        err_args, err_close = found
        close = _CLOSE.match(src, err_close + 1)
        if close is None or not _all_code(mask, close.start(), close.end()):
            continue
        if _ELSE.match(src, close.end()):
            continue
        args = [a.strip() for a in cmp_args] + [a.strip() for a in err_args]
        text = b"TEST_BYTES(err, " + b", ".join(args) + b");"
        edits.append((start, close.end(), text))
    if not edits:
        return src, 0
    out = bytearray()
    last = 0
    count = 0
    for begin, end, text in edits:
        if begin < last:  # nested inside an earlier match: keep the outer one
            continue
        out += src[last:begin] + text
        last = end
        count += 1
    out += src[last:]
    return bytes(out), count


def run(repo: Path, paths: tuple[str, ...], dry_run: bool) -> int:
    total = 0
    for path in list(paths) or sources(repo):
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        out, count = rewrite(target.read_bytes())
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would rewrite" if dry_run else "rewrote"
    print(f"testbytes: {verb} {total} memcmp checks as TEST_BYTES")
    return 0

"""``dktools testbytes``: byte comparisons in tests as TEST_BYTES checks.

Rewrites each test check of exactly one of the forms::

    if (memcmp(GOT, WANT, N) != 0) {
        ERR(FORMAT, ARGS...);
    }

    if (memcmp(GOT, WANT, N) != 0) {
        printf(FORMAT "\\n", ARGS...);
        COUNTER++;
    }

as ``TEST_BYTES(COUNTER, GOT, WANT, N, FORMAT, ARGS...);`` (ctest.h;
COUNTER is ``err`` for ERR, which counts into it), which also prints
where and how the bytes differ.  The printf form needs its format to end
in a newline, which TEST_BYTES supplies.  Only these shapes are
rewritten: an ``if`` with another condition, any other statement in its
body, or an ``else`` before or after it is left as written, as is any of
it inside a comment, a string or a preprocessor line.

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
_NOT_EQUAL_ZERO = re.compile(rb"\s*!=\s*0\s*\)\s*\{\s*(ERR|printf)\s*\(")
_CLOSE = re.compile(rb"\s*;\s*\}")
_COUNT_CLOSE = re.compile(rb"\s*;\s*([A-Za-z_]\w*)\s*\+\+\s*;\s*\}")
_ENDS_IN_NEWLINE = re.compile(rb'\\n"\s*$')
_ELSE = re.compile(rb"\s*else\b")
_AFTER_ELSE = re.compile(rb"\belse\s*$")


def _all_code(mask: bytearray, start: int, stop: int) -> bool:
    return all(mask[start:stop])


def _body_end(
    src: bytes, mask: bytearray, call: bytes, args: list[bytes], close_paren: int
) -> tuple[bytes, list[bytes], int] | None:
    """The counter, the report's format and arguments, and the end of the
    '}' for a body that is just the ERR call, or the printf call and a
    counter increment; None for any other body."""
    if call == b"ERR":
        close = _CLOSE.match(src, close_paren + 1)
        if close is None or not _all_code(mask, close.start(), close.end()):
            return None
        return b"err", args, close.end()
    close = _COUNT_CLOSE.match(src, close_paren + 1)
    if close is None or not _all_code(mask, close.start(), close.end()):
        return None
    newline = _ENDS_IN_NEWLINE.search(args[0])
    if newline is None:
        return None
    fmt = args[0][: newline.start()] + b'"'
    return close.group(1), [fmt, *args[1:]], close.end()


def rewrite(src: bytes) -> tuple[bytes, int]:
    """'src' with every single-ERR memcmp check as TEST_BYTES, and how
    many were rewritten."""
    mask = code_mask(src)
    edits: list[tuple[int, int, bytes]] = []
    for m in _IF_MEMCMP.finditer(src):
        start = m.start()
        if not _all_code(mask, start, m.end()):
            continue
        if _AFTER_ELSE.search(src, max(0, start - 16), start):
            continue  # an else-if chain keeps its braces
        open_cmp = m.end() - 1
        found = split_args(src, mask, open_cmp)
        if found is None or len(found[0]) != 3:
            continue
        cmp_args, cmp_close = found
        tail = _NOT_EQUAL_ZERO.match(src, cmp_close + 1)
        if tail is None or not _all_code(mask, tail.start(), tail.end()):
            continue
        open_report = tail.end() - 1
        found = split_args(src, mask, open_report)
        if found is None or not found[0][0].strip():
            continue
        body = _body_end(src, mask, tail.group(1), *found)
        if body is None:
            continue
        counter, report_args, end = body
        if _ELSE.match(src, end):
            continue
        args = [a.strip() for a in cmp_args] + [a.strip() for a in report_args]
        text = b"TEST_BYTES(" + counter + b", " + b", ".join(args) + b");"
        edits.append((start, end, text))
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

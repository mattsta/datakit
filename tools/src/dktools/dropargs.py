"""``dktools dropargs``: remove arguments from every call of a function.

``dktools dropargs NAME --keep 0,2 --arity 5 [--dry-run] [FILE ...]``
rewrites each call ``NAME(a0, a1, a2, a3, a4)`` in code to keep only the
listed argument positions, ``NAME(a0, a2)``: the edit an API change that
retires parameters needs at every call site (a definition's parameter
list is the caller's to change by hand).

A call is ``NAME`` as a whole identifier in code, then optional spaces
and ``(``.  Its arguments split at the top-level commas, with nested
parentheses, brackets and braces, comments, string and character
literals kept whole.  Every call must have exactly ``--arity``
arguments; one that does not is reported with its line and the file is
left unchanged, so a macro definition or a call with a different shape
is a hand edit.  The kept arguments keep their own text and the comma
spacing of the call becomes ``", "``.  Preprocessor lines (macro
definitions) are not code here, and a call of NAME nested in another's
arguments is left as written.

Files default to the tracked ``src/**/*.c`` and ``*.h``.  Prints each
file's count; exits 0, 1 when a call has another arity, 2 for bad input.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .clex import code_mask
from .gitutil import ToolError
from .retoken import sources

_IDENT = re.compile(rb"[A-Za-z_][A-Za-z0-9_]*")
_OPEN = b"([{"
_CLOSE = b")]}"


@dataclass(frozen=True)
class Mismatch:
    line: int
    arity: int


def _is_word(byte: int) -> bool:
    return chr(byte).isalnum() or byte == 0x5F


def split_args(
    src: bytes, mask: bytearray, open_paren: int
) -> tuple[list[bytes], int] | None:
    """The arguments of the call whose '(' is at 'open_paren', and the
    index of its ')'; None when the parenthesis never closes."""
    depth = 0
    start = open_paren + 1
    args: list[bytes] = []
    i = open_paren
    while i < len(src):
        if mask[i]:
            c = src[i]
            if c in _OPEN:
                depth += 1
            elif c in _CLOSE:
                depth -= 1
                if depth == 0:
                    args.append(src[start:i])
                    return args, i
            elif c == 0x2C and depth == 1:
                args.append(src[start:i])
                start = i + 1
        i += 1
    return None


def drop(
    src: bytes, name: str, keep: tuple[int, ...], arity: int
) -> tuple[bytes, int, list[Mismatch]]:
    """'src' with every call of 'name' cut to the 'keep' positions, the
    count of calls rewritten, and the calls with another arity (then
    nothing is rewritten)."""
    mask = code_mask(src)
    token = name.encode()
    edits: list[tuple[int, int, bytes]] = []
    mismatches: list[Mismatch] = []
    at = src.find(token)
    while at >= 0:
        end = at + len(token)
        whole = (
            all(mask[at:end])
            and (at == 0 or not _is_word(src[at - 1]))
            and (end == len(src) or not _is_word(src[end]))
        )
        if whole:
            j = end
            while j < len(src) and src[j] in b" \t":
                j += 1
            if j < len(src) and src[j : j + 1] == b"(" and mask[j]:
                found = split_args(src, mask, j)
                if found is not None:
                    args, close = found
                    if len(args) != arity:
                        line = src.count(b"\n", 0, at) + 1
                        mismatches.append(Mismatch(line, len(args)))
                    else:
                        kept = b", ".join(args[k].strip() for k in keep)
                        edits.append((j + 1, close, kept))
                    # a call of 'name' inside the arguments stays as it is
                    end = close
        at = src.find(token, end)
    if mismatches:
        return src, 0, mismatches
    out = bytearray()
    last = 0
    for start, stop, text in edits:
        out += src[last:start] + text
        last = stop
    out += src[last:]
    return bytes(out), len(edits), []


def parse_keep(text: str, arity: int) -> tuple[int, ...]:
    """The kept positions from '0,2'; ToolError unless each is a distinct
    position below 'arity', in ascending order."""
    try:
        keep = tuple(int(part) for part in text.split(","))
    except ValueError:
        raise ToolError(f"--keep {text!r}: not comma-separated positions") from None
    if list(keep) != sorted(set(keep)) or any(k < 0 or k >= arity for k in keep):
        raise ToolError(f"--keep {text!r}: ascending distinct positions below {arity}")
    return keep


def run(
    repo: Path,
    name: str,
    keep_text: str,
    arity: int,
    paths: tuple[str, ...],
    dry_run: bool,
) -> int:
    if not _IDENT.fullmatch(name.encode()):
        raise ToolError(f"{name!r} is not a C identifier")
    if arity < 1:
        raise ToolError("--arity must be at least 1")
    keep = parse_keep(keep_text, arity)
    total = 0
    bad = False
    for path in list(paths) or sources(repo):
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        out, count, mismatches = drop(target.read_bytes(), name, keep, arity)
        for m in mismatches:
            print(f"  {path}:{m.line}: {name} with {m.arity} arguments, not {arity}")
            bad = True
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would rewrite" if dry_run else "rewrote"
    print(f"dropargs: {verb} {total} call(s) of {name}")
    return 1 if bad else 0

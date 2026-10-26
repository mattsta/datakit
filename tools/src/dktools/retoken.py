"""``dktools retoken``: rename one C token wherever code uses it.

Replaces every whole-token occurrence (an identifier, or a number with
its suffix) of OLD with NEW in the
tracked C sources (``src/**/*.c`` and ``*.h``, the vendored
``src/memshard`` excluded) or in the files given.  Only code changes:
an OLD inside a comment, a string or a character literal stays, while
one inside a macro body (a preprocessor line) is code and changes.
``--comments`` renames it in comments too (prose naming the token);
string and character literals never change.  ``--text`` replaces an
exact text instead of a token (such as ``= {0}``), only where every
byte of it is code.
Prints each file's count; ``--dry-run`` changes nothing.

Exits 0, or 2 for a bad token or path.
"""

from __future__ import annotations

import re
from pathlib import Path

from . import gitutil
from .clex import code_mask
from .gitutil import ToolError

#: a token: an identifier, or a number with its suffix, digit separators
#: and fraction (1000000ULL, 1'000'000, 1000000.0)
IDENT = re.compile(rb"[0-9][A-Za-z0-9_']*(?:\.[0-9A-Za-z_']*)?|[A-Za-z_][A-Za-z0-9_]*")
NAME = r"[A-Za-z_][A-Za-z0-9_]*|[0-9][A-Za-z0-9_']*(?:\.[0-9A-Za-z_']*)?"

#: vendored trees whose sources stay as upstream wrote them
VENDORED = ("src/memshard/",)


def retoken(
    src: bytes, old: bytes, new: bytes, comments: bool = False
) -> tuple[bytes, int]:
    """'src' with each code occurrence (and, with 'comments', each one in a
    comment) of the token 'old' made 'new', and how many there were."""
    mask = code_mask(src, directives_are_code=True, comments_are_code=comments)
    out = bytearray()
    last = 0
    count = 0
    for m in IDENT.finditer(src):
        if m.group() != old or not all(mask[m.start() : m.end()]):
            continue
        out += src[last : m.start()]
        out += new
        last = m.end()
        count += 1
    out += src[last:]
    return bytes(out), count


def retext(src: bytes, old: bytes, new: bytes) -> tuple[bytes, int]:
    """'src' with each occurrence of the exact text 'old' that is code
    throughout (never in a comment or literal) made 'new'."""
    mask = code_mask(src, directives_are_code=True)
    out = bytearray()
    last = 0
    count = 0
    at = src.find(old)
    while at >= 0:
        if all(mask[at : at + len(old)]):
            out += src[last:at]
            out += new
            last = at + len(old)
            count += 1
            at = src.find(old, last)
        else:
            at = src.find(old, at + 1)
    out += src[last:]
    return bytes(out), count


def sources(repo: Path) -> list[str]:
    """The tracked C sources retoken works on by default."""
    listed = gitutil.split_z(
        gitutil.git(repo, "ls-files", "-z", "--", "src/*.c", "src/*.h")
    )
    return [p for p in listed if not p.startswith(VENDORED)]


def run(
    repo: Path,
    old: str,
    new: str,
    paths: tuple[str, ...],
    dry_run: bool,
    comments: bool = False,
    text: bool = False,
) -> int:
    if text:
        if not old or comments:
            raise ToolError("--text takes a non-empty OLD and no --comments")
    else:
        for token in (old, new):
            if not re.fullmatch(NAME, token):
                raise ToolError(f"{token!r} is not a C identifier or number")
    targets = list(paths) or sources(repo)
    total = 0
    for path in targets:
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        src = target.read_bytes()
        if text:
            out, count = retext(src, old.encode(), new.encode())
        else:
            out, count = retoken(src, old.encode(), new.encode(), comments)
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would rename" if dry_run else "renamed"
    print(f"retoken: {verb} {total} '{old}' to '{new}'")
    return 0

"""``dktools retoken``: rename one C token wherever code uses it.

Replaces every whole-identifier occurrence of OLD with NEW in the
tracked C sources (``src/**/*.c`` and ``*.h``, the vendored
``src/memshard`` excluded) or in the files given.  Only code changes:
an OLD inside a comment, a string or a character literal stays, while
one inside a macro body (a preprocessor line) is code and changes.
``--comments`` renames it in comments too (prose naming the token);
string and character literals never change.
Prints each file's count; ``--dry-run`` changes nothing.

Exits 0, or 2 for a bad token or path.
"""

from __future__ import annotations

import re
from pathlib import Path

from . import gitutil
from .clex import code_mask
from .gitutil import ToolError

IDENT = re.compile(rb"[A-Za-z_][A-Za-z0-9_]*")

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
) -> int:
    for token in (old, new):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token):
            raise ToolError(f"{token!r} is not a C identifier")
    targets = list(paths) or sources(repo)
    total = 0
    for path in targets:
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        src = target.read_bytes()
        out, count = retoken(src, old.encode(), new.encode(), comments)
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would rename" if dry_run else "renamed"
    print(f"retoken: {verb} {total} '{old}' to '{new}'")
    return 0

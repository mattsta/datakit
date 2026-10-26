"""``dktools enumconst``: one-constant enums become constexpr int (C23).

``enum { NAME = EXPR };`` was the pre-C23 way to name an integer constant
expression. C23's constexpr says it directly. Each such statement, where
every byte is code (macro bodies included; comments and literals never),
becomes ``constexpr int NAME = EXPR;`` inside a function or a macro body,
and ``static constexpr int NAME = EXPR;`` at file scope (one copy per
translation unit, as the enum constant was). int keeps the enum
constant's type, so no arithmetic changes.

Enums with a tag, a typedef, a fixed underlying type, several constants
or no initializer stay. Prints each file's count; ``--dry-run`` changes
nothing. Exits 0, or 2 for a missing file.
"""

from __future__ import annotations

import re
from pathlib import Path

from .clex import code_mask
from .gitutil import ToolError
from .retoken import sources

ONE_CONSTANT = re.compile(
    rb"(?<![\w])enum\s*\{\s*(?P<name>[A-Za-z_]\w*)\s*=\s*"
    rb"(?P<expr>[^{};,]*(?:\([^()]*\)[^{};,]*)*?)\s*\}\s*;"
)
TYPEDEF_BEFORE = re.compile(rb"typedef\s*$")


def _depth(src: bytes, mask: bytearray, at: int) -> int:
    """Brace depth of code at 'at' (preprocessor lines count as code)."""
    depth = 0
    for i in range(at):
        if mask[i]:
            if src[i] == 0x7B:
                depth += 1
            elif src[i] == 0x7D:
                depth -= 1
    return depth


def _in_directive(src: bytes, at: int) -> bool:
    line_start = src.rfind(b"\n", 0, at) + 1
    return src[line_start:at].lstrip().startswith(b"#")


def rewrite(src: bytes) -> tuple[bytes, int]:
    """'src' with each one-constant enum made a constexpr int, and how many."""
    mask = code_mask(src, directives_are_code=True)
    out = bytearray()
    last = 0
    count = 0
    for m in ONE_CONSTANT.finditer(src):
        if not all(mask[m.start() : m.end()]):
            continue
        if TYPEDEF_BEFORE.search(src[max(0, m.start() - 16) : m.start()]):
            continue
        name, expr = m.group("name"), m.group("expr").strip()
        file_scope = (
            not _in_directive(src, m.start()) and _depth(src, mask, m.start()) == 0
        )
        keyword = b"static constexpr int " if file_scope else b"constexpr int "
        out += src[last : m.start()] + keyword + name + b" = " + expr + b";"
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
        out, count = rewrite(target.read_bytes())
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would rewrite" if dry_run else "rewrote"
    print(f"enumconst: {verb} {total} one-constant enum(s)")
    return 0

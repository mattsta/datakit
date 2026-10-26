"""``dktools dropinclude``: remove one system header's include lines.

Deletes each ``#include <HEADER>`` line (an optional trailing comment
allowed) from the tracked C sources (vendored ``src/memshard``
excluded) or from the files given, for a header the language has made
unnecessary, such as ``stdbool.h`` under C23.  Prints each file's
count; ``--dry-run`` changes nothing.

Exits 0, or 2 for a bad header name or path.
"""

from __future__ import annotations

import re
from pathlib import Path

from .gitutil import ToolError
from .retoken import sources


def include_line(header: str) -> re.Pattern[bytes]:
    """The pattern of an include line for 'header', newline included."""
    return re.compile(
        rb"^[ \t]*#[ \t]*include[ \t]*<"
        + re.escape(header.encode())
        + rb">[ \t]*(?:/\*[^\n]*\*/|//[^\n]*)?[ \t]*\n",
        re.MULTILINE,
    )


def drop(src: bytes, header: str) -> tuple[bytes, int]:
    """'src' without its include lines for 'header', and how many."""
    return include_line(header).subn(b"", src)


def run(repo: Path, header: str, paths: tuple[str, ...], dry_run: bool) -> int:
    if not re.fullmatch(r"[A-Za-z0-9_./-]+\.h", header) or ".." in header:
        raise ToolError(f"{header!r} is not a header name")
    targets = list(paths) or sources(repo)
    total = 0
    for path in targets:
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        out, count = drop(target.read_bytes(), header)
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would remove" if dry_run else "removed"
    print(f"dropinclude: {verb} {total} '<{header}>' includes")
    return 0

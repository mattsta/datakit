"""``dktools wrapcalls``: wrap the calls a build says drop a result.

Reads a build log for clang's "ignoring return value of type 'T'
declared with 'nodiscard' attribute" warnings and wraps each reported
call in a macro, ``call(...);`` becoming ``MACRO(call(...));``, so a
test can check every result it used to drop.

A site is wrapped only when the call is a whole statement: nothing but
whitespace before it on its line, the previous code character one that
ends a statement or opens a block (``; { } ) :``), and ``;`` right after
its closing parenthesis.  Anything else (an assignment split over
lines, a call inside an expression, a call in a macro body) is listed
as skipped, for a hand edit.  Comments, string and character literals
and preprocessor lines are skipped while matching parentheses and
looking back.  Columns are byte columns, as the compiler reports them.

Exits 0 when every site was wrapped, 1 when any was skipped.
"""

from __future__ import annotations

import os
import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from .clex import code_mask

WARNING = re.compile(
    r"^(?P<path>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): warning: ignoring "
    r"return value of type '(?P<type>[^']+)' declared with 'nodiscard'"
)

#: verify and validate build a copy of the repository under
#: .../verify/NAME/src/; a log path inside one maps back to the repo
SCRATCH = re.compile(r"/verify/[^/]+/src/(?P<rest>.+)$")

#: characters that may come right before a statement
STATEMENT_BEFORE = b";{}):"


@dataclass(frozen=True, order=True)
class Site:
    path: str  # repo-relative
    line: int
    col: int


@dataclass(frozen=True)
class Skip:
    site: Site
    reason: str
    text: str


def repo_path(logged: str, repo: Path) -> str | None:
    """The repo-relative path of a path in a build log, or None."""
    norm = os.path.normpath(logged)
    found = SCRATCH.search(norm)
    if found:
        return found["rest"]
    try:
        rel = Path(norm).resolve().relative_to(repo.resolve())
    except ValueError:
        return None
    return None if rel.parts[:1] == ("build",) else rel.as_posix()


def sites(lines: Iterable[str], type_name: str, repo: Path) -> list[Site]:
    """Sorted, distinct sites in 'lines' that drop a 'type_name'."""
    found: set[Site] = set()
    for line in lines:
        hit = WARNING.match(line)
        if not hit or hit["type"] != type_name:
            continue
        path = repo_path(hit["path"], repo)
        if path is not None:
            found.add(Site(path, int(hit["line"]), int(hit["col"])))
    return sorted(found)


def _call_end(src: bytes, mask: bytearray, start: int) -> int | None:
    """The offset just past the ')' closing the call at 'start', or None
    when no call starts there."""
    i = start
    while i < len(src) and (src[i : i + 1].isalnum() or src[i] == 0x5F):
        i += 1
    if i == start:
        return None
    while i < len(src) and src[i : i + 1] in (b" ", b"\t"):
        i += 1
    if i >= len(src) or src[i] != 0x28:
        return None
    depth = 0
    while i < len(src):
        if mask[i]:
            if src[i] == 0x28:
                depth += 1
            elif src[i] == 0x29:
                depth -= 1
                if depth == 0:
                    return i + 1
        i += 1
    return None


def plan(src: bytes, site: Site, macro: str) -> tuple[int, int] | str:
    """(call start, call end) to wrap, or the reason the site is skipped."""
    lines = src.split(b"\n")
    if site.line > len(lines):
        return "line past the end of the file"
    line_start = sum(len(lines[k]) + 1 for k in range(site.line - 1))
    start = line_start + site.col - 1
    if start >= len(src) or src[line_start:start].strip(b" \t"):
        return "not at the start of its line"
    mask = code_mask(src)
    before = start - 1
    while before >= 0 and (not mask[before] or src[before : before + 1].isspace()):
        before -= 1
    if before >= 0 and src[before : before + 1] not in [
        bytes([b]) for b in STATEMENT_BEFORE
    ]:
        return "continues an expression"
    end = _call_end(src, mask, start)
    if end is None:
        return "no call at the reported column"
    if src[end : end + 1] != b";":
        return "not a whole statement"
    if src[start : start + len(macro) + 1] == macro.encode() + b"(":
        return "already wrapped"
    return (start, end)


def wrap_file(
    src: bytes, file_sites: list[Site], macro: str
) -> tuple[bytes, list[Site], list[Skip]]:
    """'src' with every wrappable site wrapped, the wrapped sites and the
    skipped ones."""
    edits: list[tuple[int, int, Site]] = []
    skipped: list[Skip] = []
    lines = src.split(b"\n")
    for site in file_sites:
        got = plan(src, site, macro)
        if isinstance(got, str):
            text = lines[site.line - 1] if site.line <= len(lines) else b""
            skipped.append(Skip(site, got, text.decode(errors="replace").strip()))
        else:
            edits.append((got[0], got[1], site))
    out = src
    for start, end, _ in sorted(edits, reverse=True):
        out = out[:start] + macro.encode() + b"(" + out[start:end] + b")" + out[end:]
    return out, [e[2] for e in edits], skipped


def run(
    repo: Path,
    log: Path,
    type_name: str,
    macro: str,
    dry_run: bool,
    paths: tuple[str, ...] = (),
) -> int:
    """Wrap the sites in 'log' (only those in 'paths', when given)."""
    found = sites(log.read_text(errors="replace").splitlines(), type_name, repo)
    if paths:
        found = [s for s in found if s.path in paths]
    by_file: dict[str, list[Site]] = {}
    for site in found:
        by_file.setdefault(site.path, []).append(site)
    wrapped = 0
    skipped: list[Skip] = []
    for path, file_sites in sorted(by_file.items()):
        target = repo / path
        out, done, skips = wrap_file(target.read_bytes(), file_sites, macro)
        wrapped += len(done)
        skipped.extend(skips)
        if done and not dry_run:
            target.write_bytes(out)
    verb = "would wrap" if dry_run else "wrapped"
    print(
        f"wrapcalls: {verb} {wrapped} of {len(found)} '{type_name}' sites "
        f"in {len(by_file)} files with {macro}"
    )
    for skip in skipped:
        s = skip.site
        print(f"  skipped {s.path}:{s.line}:{s.col}: {skip.reason}: {skip.text}")
    return 1 if skipped else 0

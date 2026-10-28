"""``dktools docgaps``: public functions no user-level doc mentions.

``dktools docgaps [HEADER ...] [--names]``

Every function a public header under ``src/`` declares (the headers and
declarations ``dktools coverage`` counts as public API) is looked for, as
a whole word, in the user-level docs: ``docs/**/*.md`` and ``README.md``
(suite and benchmark entry points, ``fooTest`` / ``fooBench``, are not
API and are not counted).
Prints one line per header with undocumented functions, most undocumented
first, then the totals; ``--names`` (or naming HEADERs) lists each
header's undocumented functions too.  Without HEADERs it also names the
module docs (``docs/modules/**/*.md``) that ``docs/INDEX.md`` does not
link.  Docs are written from this report,
so it gates nothing: it exits 0 (2 for a HEADER that is not public).
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .coverage import collect_headers, declared_functions

_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

#: A suite's or benchmark's entry point (``fooTest(argc, argv)``), not API
_ENTRY_POINT = re.compile(r"(Test|Bench)$")


@dataclass(frozen=True)
class HeaderGaps:
    header: str
    declared: int
    undocumented: tuple[str, ...]

    @property
    def documented(self) -> int:
        return self.declared - len(self.undocumented)


def doc_words(repo: Path) -> set[str]:
    """Every identifier-shaped word of the user-level docs."""
    paths = sorted((repo / "docs").rglob("*.md"))
    readme = repo / "README.md"
    if readme.exists():
        paths.append(readme)
    words: set[str] = set()
    for path in paths:
        words.update(_WORD.findall(path.read_text(encoding="utf-8", errors="replace")))
    return words


def gaps(headers: dict[str, str], words: set[str]) -> list[HeaderGaps]:
    """Per header with declarations: its functions no doc names, sorted
    most undocumented first (then by path)."""
    out = []
    for path, text in headers.items():
        names = sorted(
            {n for n in declared_functions(text) if not _ENTRY_POINT.search(n)}
        )
        if names:
            missing = tuple(n for n in names if n not in words)
            out.append(HeaderGaps(path, len(names), missing))
    return sorted(out, key=lambda g: (-len(g.undocumented), g.header))


def unindexed(repo: Path) -> list[str]:
    """Module docs (docs/modules/**/*.md) that docs/INDEX.md never links."""
    index = repo / "docs" / "INDEX.md"
    text = index.read_text(encoding="utf-8") if index.exists() else ""
    docs = sorted((repo / "docs" / "modules").rglob("*.md"))
    return [
        p.relative_to(repo).as_posix()
        for p in docs
        if p.relative_to(repo / "docs").as_posix() not in text
    ]


def render(report: list[HeaderGaps], names: bool, loose: list[str] = ()) -> str:
    lines = [f"  not in docs/INDEX.md: {path}" for path in loose]
    for g in report:
        if not g.undocumented:
            continue
        lines.append(f"{len(g.undocumented):5d} of {g.declared:4d}  {g.header}")
        if names:
            lines += [f"        {n}" for n in g.undocumented]
    declared = sum(g.declared for g in report)
    missing = sum(len(g.undocumented) for g in report)
    complete = sum(1 for g in report if not g.undocumented)
    lines.append(
        f"docgaps: {missing} of {declared} public functions undocumented; "
        f"{complete} of {len(report)} headers fully documented; "
        f"{len(loose)} module docs not in docs/INDEX.md"
    )
    return "\n".join(lines)


def run(repo: Path, wanted: tuple[str, ...], names: bool) -> int:
    headers = collect_headers(repo)
    if wanted:
        unknown = [h for h in wanted if h not in headers]
        if unknown:
            print(f"docgaps: not a public header: {', '.join(unknown)}")
            return 2
        headers = {h: headers[h] for h in wanted}
    loose = [] if wanted else unindexed(repo)
    print(render(gaps(headers, doc_words(repo)), names or bool(wanted), loose))
    return 0

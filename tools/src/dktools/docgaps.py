"""``dktools docgaps``: public functions no user-level doc mentions.

``dktools docgaps [HEADER ...] [--names]``

Every function a public header under ``src/`` declares (the headers and
declarations ``dktools coverage`` counts as public API) is looked for, as
a whole word, in the user-level docs: ``docs/**/*.md`` and ``README.md``
(suite and benchmark entry points, ``fooTest`` / ``fooBench``, and
declarations under ``#ifdef DATAKIT_TEST`` are not API and are not
counted).
Typed families are documented once: a doc that writes ``prefix<T>rest``
(``segment<T>NewFromArray``) documents ``prefix`` + every type + ``rest``,
and one that writes ``prefix<T>`` (``fenwick<T>``) lets a type's function
count as documented when the same function of another type is
(``fenwickU32Query`` by ``fenwickI64Query``).  The types are the integer
widths (``I8``..``I128``, ``U8``..``U128``), ``Float`` and ``Double``.

A doc's ``<s>Rest`` token (``<s>NewP``: the call every persistence
wrapper has, named after its structure) documents every name ending in
``Rest``.

A tier header (``fooSmall.h``, ``fooMedium.h``, ``fooLarge.h`` or
``fooFull.h`` beside a public ``foo.h``) is used through its umbrella, so
it is left out of the list and the totals and summed on a line of its
own; naming one as a HEADER reports it like any other.

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

from .coverage import collect_headers, declared_functions, test_lines

_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

#: A suite's or benchmark's entry point (``fooTest(argc, argv)``), not API
_ENTRY_POINT = re.compile(r"(Test|Bench)$")

#: ``prefix<T>rest`` in a doc: a typed family's name pattern
_FAMILY = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)<T>([A-Za-z0-9_]*)")

#: ``<s>Rest`` in a doc: a call every structure has, named after it
_PER_STRUCTURE = re.compile(r"<s>([A-Z][A-Za-z0-9_]*)")

#: The type tags a typed family's names carry after their prefix
_TYPE_TAG = re.compile(r"(?:[IU](?:8|16|32|64|128)|Float|Double)")
_TYPE_TAGS = tuple(
    [f"{s}{w}" for s in "IU" for w in (8, 16, 32, 64, 128)] + ["Float", "Double"]
)

#: A tier header's name: its umbrella's stem and the tier
_TIER = re.compile(r"^(.*?)(Small|Medium|Large|Full)\.h$")


@dataclass(frozen=True)
class HeaderGaps:
    header: str
    declared: int
    undocumented: tuple[str, ...]
    tier: bool = False

    @property
    def documented(self) -> int:
        return self.declared - len(self.undocumented)


@dataclass(frozen=True)
class DocIndex:
    """What the user-level docs name: whole words, typed families
    (``prefix<T>rest`` patterns; a bare ``prefix<T>`` has rest '') and
    per-structure calls (the Rest of ``<s>Rest``)."""

    words: frozenset[str]
    families: frozenset[tuple[str, str]] = frozenset()
    per_structure: frozenset[str] = frozenset()

    def names(self, name: str) -> bool:
        """Whether the docs document 'name': as a word, through a
        ``prefix<T>rest`` pattern, as a sibling of a documented type of a
        ``prefix<T>`` family, or as a structure's ``<s>Rest`` call."""
        if name in self.words:
            return True
        if any(
            name.endswith(rest) and len(name) > len(rest) for rest in self.per_structure
        ):
            return True
        for prefix, rest in self.families:
            if not name.startswith(prefix):
                continue
            tag = _TYPE_TAG.match(name, len(prefix))
            if not tag:
                continue
            suffix = name[tag.end() :]
            if rest and suffix == rest:
                return True
            if not rest and any(
                prefix + t + suffix in self.words or (prefix, suffix) in self.families
                for t in _TYPE_TAGS
            ):
                return True
        return False


def doc_index(repo: Path) -> DocIndex:
    """The words and typed families of the user-level docs."""
    paths = sorted((repo / "docs").rglob("*.md"))
    readme = repo / "README.md"
    if readme.exists():
        paths.append(readme)
    words: set[str] = set()
    families: set[tuple[str, str]] = set()
    per_structure: set[str] = set()
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        words.update(_WORD.findall(text))
        families.update(_FAMILY.findall(text))
        per_structure.update(_PER_STRUCTURE.findall(text))
    return DocIndex(frozenset(words), frozenset(families), frozenset(per_structure))


def is_tier(path: str, headers: dict[str, str]) -> bool:
    """Whether 'path' is a tier of a public umbrella header."""
    m = _TIER.match(path)
    return bool(m) and f"{m.group(1)}.h" in headers


def api_text(header_text: str) -> str:
    """The header without its test-only code (``#ifdef DATAKIT_TEST``
    blocks): those declarations are test hooks, not API."""
    test = test_lines(header_text)
    return "\n".join(
        line for n, line in enumerate(header_text.splitlines(), 1) if n not in test
    )


def gaps(
    headers: dict[str, str], index: DocIndex, tiers: bool = True
) -> list[HeaderGaps]:
    """Per header with declarations: its functions no doc names, sorted
    most undocumented first (then by path); with 'tiers', tier headers
    are marked as such."""
    out = []
    for path, text in headers.items():
        names = sorted(
            {
                n
                for n in declared_functions(api_text(text))
                if not _ENTRY_POINT.search(n)
            }
        )
        if names:
            missing = tuple(n for n in names if not index.names(n))
            tier = tiers and is_tier(path, headers)
            out.append(HeaderGaps(path, len(names), missing, tier))
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
    api = [g for g in report if not g.tier]
    tiers = [g for g in report if g.tier]
    for g in api:
        if not g.undocumented:
            continue
        lines.append(f"{len(g.undocumented):5d} of {g.declared:4d}  {g.header}")
        if names:
            lines += [f"        {n}" for n in g.undocumented]
    if tiers:
        lines.append(
            f"  tier headers (used through their umbrella, not counted): "
            f"{len(tiers)} headers, "
            f"{sum(len(g.undocumented) for g in tiers)} functions no doc names"
        )
    declared = sum(g.declared for g in api)
    missing = sum(len(g.undocumented) for g in api)
    complete = sum(1 for g in api if not g.undocumented)
    lines.append(
        f"docgaps: {missing} of {declared} public functions undocumented; "
        f"{complete} of {len(api)} headers fully documented; "
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
    report = gaps(headers, doc_index(repo), tiers=not wanted)
    print(render(report, names or bool(wanted), loose))
    return 0

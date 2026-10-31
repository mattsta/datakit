"""``dktools docgaps``: public functions no user-level doc mentions.

``dktools docgaps [HEADER ...] [--names]``

Every function a public header under ``src/`` declares (the headers and
declarations ``dktools coverage`` counts as public API) is looked for, as
a whole word, in the user-level docs: ``docs/**/*.md`` and ``README.md``
(suite and benchmark entry points, ``fooTest`` / ``fooBench``,
declarations under ``#ifdef DATAKIT_TEST``, and system functions a header
redeclares, libc's allocators, ``pthread_*`` and reserved ``__`` names,
are not API and are not
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

``--stale`` turns the report around: the calls the docs' C code blocks
make (``name(``) whose name starts like a public function's (its leading
lowercase run, ``flex`` of ``flexNew``) but that no source file under
``src/`` names, nor a function the block itself defines or declares: a renamed or
removed call the docs still show.  Inline code spans in the prose
(`` `name` ``; not a glob, ``name*``) are read the same way; a
camelCase name only, as datakit's API is (``timer_create`` is libc's),
not a brace shorthand (``fooStream{Init,Final}``), and not on a line
saying the name does not exist ("there is no `fooDestroy`");
and a source file they name
(`` `foo.h` ``, `` `foo.c` ``; one whose name starts like a public
function's, outside a span holding a ``<...>`` or ``*`` placeholder) must
exist under ``src/`` or ``deps/``.
Typed names count when one of their family's types is in the source
(``fenwickI64Update`` by ``fenwick##T##Update`` users naming
``fenwickU32Update``).  It exits 1 when there are any.

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
from .retoken import VENDORED

_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

#: A suite's or benchmark's entry point (``fooTest(argc, argv)``), not API
_ENTRY_POINT = re.compile(r"(Test|Bench)$")

#: A system function a header redeclares (libc's allocators, pthreads, a
#: reserved name such as the coverage runtime's), not datakit's API
_SYSTEM = re.compile(
    r"^(pthread_\w+|posix_memalign|aligned_alloc|malloc|calloc|realloc|free|__\w+)$"
)

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
                if not _ENTRY_POINT.search(n) and not _SYSTEM.match(n)
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


#: A fenced C block in a doc
_C_BLOCK = re.compile(r"^```c[^\n]*\n(.*?)^```", re.MULTILINE | re.DOTALL)

#: A call in a code block
_CALL = re.compile(r"\b([a-z][A-Za-z0-9_]*)\s*\(")

#: A line defining or declaring a function in a code block: a type, the
#: name and its parameters, then a body, a semicolon or the end of the line
#: (a comment may follow); a statement (return, else, case) is not one
_DEFINED = re.compile(
    r"^\s*(?!(?:return|else|case)\b)(?:[A-Za-z_][\w\s\*]*?[\s\*])([a-z]\w*)"
    r"\s*\([^;]*(?:\)\s*;)?\s*(?:/\*.*?\*/)?\s*$",
    re.MULTILINE,
)

#: An inline code span in prose
_SPAN = re.compile(r"`([^`\n]+)`")

#: A line telling the reader a name does not exist
_ABSENT = re.compile(r"\b(?:[Tt]here is no|does not exist|[Nn]o such)\b|\bno\s+`")

#: A source file a span names
_SOURCE_FILE = re.compile(r"\b([A-Za-z_][\w/]*\.[ch])\b")

#: The leading lowercase run of a name (``flex`` of ``flexNew``)
_STEM = re.compile(r"^[a-z][a-z0-9]*")


@dataclass(frozen=True)
class StaleCall:
    doc: str
    line: int
    name: str


def _source_words(repo: Path) -> set[str]:
    """Every word of every source file under src/ (vendored code too: the
    docs may call it)."""
    words: set[str] = set()
    for path in (repo / "src").rglob("*"):
        if path.suffix in (".h", ".c"):
            words.update(
                _WORD.findall(path.read_text(encoding="utf-8", errors="replace"))
            )
    return words


def _known(name: str, words: set[str]) -> bool:
    """Whether the source names 'name', or a sibling of it in a typed
    family (the same name with another type tag)."""
    if name in words:
        return True
    tag = next((t for t in _TYPE_TAGS if t in name), None)
    return tag is not None and any(
        name.replace(tag, other, 1) in words for other in _TYPE_TAGS if other != tag
    )


def stale_calls(repo: Path, headers: dict[str, str]) -> list[StaleCall]:
    """The calls in the docs' C code blocks that no source file names."""
    stems = {
        m.group(0)
        for text in headers.values()
        for name in declared_functions(api_text(text))
        if not _SYSTEM.match(name) and (m := _STEM.match(name)) and len(m.group(0)) > 2
    }
    words = _source_words(repo)
    files = {
        p.name
        for base in ("src", "deps")
        if (repo / base).is_dir()
        for p in (repo / base).rglob("*")
        if p.suffix in (".h", ".c")
    }
    paths = sorted((repo / "docs").rglob("*.md"))
    if (repo / "README.md").exists():
        paths.append(repo / "README.md")
    out: list[StaleCall] = []
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        for block in _C_BLOCK.finditer(text):
            body = block.group(1)
            defined = set(_DEFINED.findall(body))
            first = text.count("\n", 0, block.start(1)) + 1
            for call in _CALL.finditer(body):
                name = call.group(1)
                stem = _STEM.match(name)
                if (
                    stem
                    and "_" not in name
                    and stem.group(0) in stems
                    and name not in defined
                    and not _known(name, words)
                ):
                    line = first + body.count("\n", 0, call.start())
                    out.append(StaleCall(path.relative_to(repo).as_posix(), line, name))
        prose = _C_BLOCK.sub(lambda m: "\n" * m.group(0).count("\n"), text)
        prose = re.sub(
            r"^```.*?^```",
            lambda m: "\n" * m.group(0).count("\n"),
            prose,
            flags=re.MULTILINE | re.DOTALL,
        )
        prose_lines = prose.splitlines()
        for span in _SPAN.finditer(prose):
            line = prose.count("\n", 0, span.start()) + 1
            body = span.group(1)
            # A line saying the name does not exist names it on purpose
            said = prose_lines[line - 1] if line <= len(prose_lines) else ""
            if _ABSENT.search(said):
                continue
            for f in _SOURCE_FILE.findall(body):
                base = f.rsplit("/", 1)[-1]
                stem = _STEM.match(base)
                if (
                    "<" not in body
                    and "*" not in body
                    and stem
                    and stem.group(0) in stems
                    and base not in files
                ):
                    out.append(StaleCall(path.relative_to(repo).as_posix(), line, f))
            for name in _WORD.findall(_SOURCE_FILE.sub(" ", body)):
                stem = _STEM.match(name)
                if (
                    stem
                    and "_" not in name
                    and f"{name}*" not in body
                    and f"{name}{{" not in body
                    and stem.group(0) in stems
                    and stem.group(0) != name
                    and not _known(name, words)
                ):
                    out.append(StaleCall(path.relative_to(repo).as_posix(), line, name))
    return out


def run(repo: Path, wanted: tuple[str, ...], names: bool, stale: bool = False) -> int:
    # The vendored allocator documents its own API upstream
    headers = {
        p: t for p, t in collect_headers(repo).items() if not p.startswith(VENDORED)
    }
    if wanted:
        unknown = [h for h in wanted if h not in headers]
        if unknown:
            print(f"docgaps: not a public header: {', '.join(unknown)}")
            return 2
        headers = {h: headers[h] for h in wanted}
    if stale:
        calls = stale_calls(repo, headers)
        for c in calls:
            print(f"{c.doc}:{c.line}: {c.name}")
        print(f"docgaps --stale: {len(calls)} calls no source names")
        return 1 if calls else 0
    loose = [] if wanted else unindexed(repo)
    report = gaps(headers, doc_index(repo), tiers=not wanted)
    print(render(report, names or bool(wanted), loose))
    return 0

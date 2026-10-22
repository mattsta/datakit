"""The leakage gate: terms that must not enter datakit's history.

Usage::

    dktools leakcheck [--staged | --range A..B | --tree] [--include-vendored]

``--staged`` (the default) checks the added lines of the staged diff,
``--range A..B`` the added lines of ``git diff A..B`` (``A...B`` diffs
from the merge base), and ``--tree`` every line of every tracked file
under ``TREE_DIRS`` plus the top-level ``*.md`` files.  Vendored code
(``VENDORED_DIRS``) mirrors its upstream exactly and is skipped unless
``--include-vendored`` is given; ``SELF_FILES`` (this module and its
tests, which must spell the terms out) are always skipped, and
``QUOTED_OUTPUT`` lets a named file contain a named term because it
quotes another program's fixed output.

Each hit prints as ``path:line: [term] text``.  ``HARD_TERMS`` fail the
gate; ``REVIEW_TERMS`` are known to match ordinary code too, so their
hits print under a separate REVIEW heading for a human to judge and do
not change the exit code.  Exits 0 with no hard hit, 1 with any, and
2 on an input error.

The term list is the commit-hygiene plan's leakage gate, copied here as
the single source of truth: names of downstream projects and their
internals, project-management and experiment labels, scratch-file
timestamps, and local filesystem paths.  Matching is case-insensitive.
A multi-word term matches with a space, hyphen, underscore or nothing
between its words (``cross shard``, ``cross-shard``, ``crossShard``).
Two review patterns stay case-sensitive because only the upper-case
form is a label: ``P[0-9]`` (a priority tag, where ``p1`` is a common
variable) and ``ARM [A-Z]`` (an experiment arm, where ``arm a`` is
prose).  ``notes/`` is the internal notes directory, so it does not
match inside a longer path or URL (``example.com/notes/``), and ``the
product`` does not match ``the production``.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from pathlib import Path

from .gitutil import ToolError, git, split_z

#: Separator allowed between the words of a multi-word term.
_SEP = r"[-_ ]?"


@dataclass(frozen=True)
class Term:
    """One gate term: its label, compiled pattern, and bucket."""

    label: str
    pattern: re.Pattern[str]
    hard: bool


def _words(*words: str) -> str:
    return _SEP.join(words)


def _term(label: str, regex: str, hard: bool, case: bool = False) -> Term:
    flags = 0 if case else re.IGNORECASE
    return Term(label, re.compile(regex, flags), hard)


#: Terms whose every hit fails the gate.
HARD_TERMS: tuple[Term, ...] = tuple(
    _term(label, regex, hard=True)
    for label, regex in (
        ("carrier", r"carrier"),
        ("zvm", r"zvm"),
        ("kraft", r"kraft"),
        ("cross shard", _words("cross", "shard")),
        ("apply thread", _words("apply", "thread")),
        ("info bus", _words("info", "bus")),
        ("fence probe", _words("fence", "probe")),
        ("CP_DIAG", _words("cp", "diag")),
        ("tsan ladder", _words("tsan", "ladder")),
        ("release flavour", _words("release", "flavou?r")),
        ("exit-wait pin", _words("exit", "wait", "pin")),
        ("tripwire", _words("trip", "wire")),
        ("src/hierarchy", r"src/hierarchy"),
        ("src/zvm", r"src/zvm"),
        ("TLS ticket", _words("tls", "ticket")),
        ("either tree", _words("either", "tree")),
        ("both trees", _words("both", "trees")),
        ("shipped reader", _words("shipped", "reader")),
        ("first consumer", _words("first", "consumer")),
        ("membership section", _words("membership", "section")),
        ("recv owner", _words("recv", "owner")),
        ("send owner", _words("send", "owner")),
        ("INV-X", r"\binv-[a-z]"),
        ("notes/", r"(?<![\w./-])notes/"),
        ("vertical N", r"\bvertical[-_ ][0-9]"),
        ("design §", r"design[-_ ]?§"),
        ("timestamp", r"20[0-9]{6}_[0-9]{6}"),
        ("/Users/", r"/Users/"),
        ("/home/", r"/home/"),
        ("/tmp/", r"/tmp/"),
    )
)

#: Terms that also match ordinary code: reported, never failing.
REVIEW_TERMS: tuple[Term, ...] = (
    _term("P<digit>", r"\bP[0-9]\b", hard=False, case=True),
    _term("arm N", r"\barm [0-9]+'?", hard=False),
    _term("ARM X", r"\bARM [A-Z]\b", hard=False, case=True),
    _term("the product", r"\b" + _words("the", "product") + r"\b", hard=False),
)

TERMS: tuple[Term, ...] = HARD_TERMS + REVIEW_TERMS

#: Tracked directories --tree scans (plus the top-level *.md files).
TREE_DIRS = ("src", "docs", "include", "tools")

#: Vendored trees: upstream's content, mirrored as-is.
VENDORED_DIRS = ("deps/", "src/memshard/")

#: Files that must spell the terms out: the gate and its tests.
SELF_FILES = ("tools/src/dktools/leakcheck.py", "tools/tests/test_leakcheck.py")

#: Terms a file may contain because it quotes another program's fixed
#: output, not a local path or a name: path -> term labels.  Every other
#: term still applies to the file, and the terms still apply everywhere
#: else.
QUOTED_OUTPUT: dict[str, tuple[str, ...]] = {
    # macOS dsymutil names the linker's temporary LTO object /tmp/lto.o;
    # dktools recognises that note and tests it
    "tools/src/dktools/steps.py": ("/tmp/",),
    "tools/tests/test_verify.py": ("/tmp/",),
}


@dataclass(frozen=True)
class Hit:
    path: str
    line: int
    term: Term
    text: str


def scan(path: str, lines: Iterable[tuple[int, str]]) -> list[Hit]:
    """Every term match in (line number, text) pairs of `path`, except the
    terms QUOTED_OUTPUT allows in that file."""
    allowed = QUOTED_OUTPUT.get(path, ())
    hits: list[Hit] = []
    for number, text in lines:
        for term in TERMS:
            if term.label not in allowed and term.pattern.search(text):
                hits.append(Hit(path, number, term, text))
    return hits


def skipped(path: str, include_vendored: bool) -> bool:
    if path in SELF_FILES:
        return True
    return not include_vendored and path.startswith(VENDORED_DIRS)


_ESCAPES = {
    "a": 7,
    "b": 8,
    "t": 9,
    "n": 10,
    "v": 11,
    "f": 12,
    "r": 13,
    '"': 34,
    "\\": 92,
}


def _unquote(name: str) -> str:
    """Undo git's C-style quoting of a path ("a\\tb", octal bytes)."""
    if not (len(name) >= 2 and name.startswith('"') and name.endswith('"')):
        return name
    body = name[1:-1]
    out = bytearray()
    i = 0
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            if nxt in "01234567" and i + 3 < len(body) + 1:
                out.append(int(body[i + 1 : i + 4], 8))
                i += 4
                continue
            out.append(_ESCAPES.get(nxt, ord(nxt)))
            i += 2
            continue
        out.extend(ch.encode("utf-8"))
        i += 1
    return out.decode("utf-8", "replace")


def added_lines(diff: str) -> Iterator[tuple[str, int, str]]:
    """(path, new line number, text) for each added line of a diff."""
    path: str | None = None
    in_header = False
    number = 0
    for line in diff.splitlines():
        if line.startswith("diff --git "):
            in_header = True
            path = None
            continue
        if in_header:
            if line.startswith("+++ "):
                target = _unquote(line[4:])
                path = None if target == "/dev/null" else target.removeprefix("b/")
                continue
            if line.startswith("@@ "):
                in_header = False
            else:
                continue
        if line.startswith("@@ "):
            m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
            number = int(m.group(1)) if m else 0
            continue
        if path is None:
            continue
        if line.startswith("+"):
            yield path, number, line[1:]
            number += 1
        elif line.startswith(" "):
            number += 1


def diff_hits(
    repo: Path, diff_args: list[str], include_vendored: bool
) -> tuple[list[Hit], int]:
    """Hits in the added lines of ``git diff DIFF_ARGS``; (hits, lines)."""
    out = git(
        repo,
        "-c",
        "core.quotePath=false",
        "diff",
        "-U0",
        "--no-color",
        "--no-ext-diff",
        "--find-renames",
        "--src-prefix=a/",
        "--dst-prefix=b/",
        *diff_args,
    )
    by_path: dict[str, list[tuple[int, str]]] = {}
    for path, number, text in added_lines(out.decode("utf-8", "replace")):
        if not skipped(path, include_vendored):
            by_path.setdefault(path, []).append((number, text))
    hits: list[Hit] = []
    for path, lines in by_path.items():
        hits.extend(scan(path, lines))
    return hits, sum(len(v) for v in by_path.values())


def tree_files(repo: Path, include_vendored: bool) -> list[str]:
    """Tracked files --tree scans (deps/ too when vendored code is in)."""
    dirs = TREE_DIRS + (("deps",) if include_vendored else ())
    out = git(repo, "ls-files", "-z", "--", *dirs)
    files = split_z(out)
    top = split_z(git(repo, "ls-files", "-z", "--", ":(glob)*.md"))
    paths = sorted(set(files) | set(top))
    return [p for p in paths if not skipped(p, include_vendored)]


def tree_hits(repo: Path, include_vendored: bool) -> tuple[list[Hit], int]:
    """Hits in every line of the --tree files; (hits, lines scanned)."""
    hits: list[Hit] = []
    total = 0
    for path in tree_files(repo, include_vendored):
        try:
            data = (repo / path).read_bytes()
        except (FileNotFoundError, IsADirectoryError):
            continue  # deleted in the working tree
        if b"\0" in data:
            continue  # binary
        lines = data.decode("utf-8", "replace").splitlines()
        total += len(lines)
        hits.extend(scan(path, enumerate(lines, start=1)))
    return hits, total


def check(
    repo: Path, mode: str, rev_range: str | None = None, include_vendored: bool = False
) -> tuple[list[Hit], int]:
    """Hits and lines scanned for `mode` ("staged", "range", "tree")."""
    if mode == "staged":
        return diff_hits(repo, ["--cached"], include_vendored)
    if mode == "range":
        if not rev_range or ".." not in rev_range or rev_range.startswith("-"):
            raise ToolError(f"--range wants A..B or A...B (got {rev_range!r})")
        return diff_hits(repo, [rev_range, "--"], include_vendored)
    if mode == "tree":
        return tree_hits(repo, include_vendored)
    raise ToolError(f"unknown leakcheck mode {mode!r}")


def format_hit(hit: Hit) -> str:
    text = hit.text.strip()
    if len(text) > 160:
        text = text[:157] + "..."
    return f"{hit.path}:{hit.line}: [{hit.term.label}] {text}"


def report(hits: list[Hit], scanned: int, out=None) -> int:
    """Print hits (hard, then review) and the summary; return exit code."""
    out = out or sys.stdout
    hard = [h for h in hits if h.term.hard]
    review = [h for h in hits if not h.term.hard]
    if hard:
        print("LEAK", file=out)
        for hit in hard:
            print(format_hit(hit), file=out)
    if review:
        print("REVIEW (does not fail the gate)", file=out)
        for hit in review:
            print(format_hit(hit), file=out)
    print(
        f"leakcheck: {len(hard)} hard hit(s), {len(review)} review hit(s), "
        f"{scanned} line(s) scanned",
        file=out,
    )
    return 1 if hard else 0


def run(
    repo: Path, mode: str, rev_range: str | None = None, include_vendored: bool = False
) -> int:
    """The CLI entry: check, print, return the exit code."""
    hits, scanned = check(repo, mode, rev_range, include_vendored)
    return report(hits, scanned)

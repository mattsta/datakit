"""``dktools spell``: misspellings in the tracked sources, docs and tools.

Runs codespell (a locked tools dependency) over the tracked C sources
(vendored ``src/memshard`` excluded), the CMake files, ``docs/``
Markdown, the root README and the tools' own sources, tests and README,
or over the files given.  Words that are right here although codespell
flags them (identifiers, domain terms) go one per line, lowercase, in
``tools/spell-words.txt``.

Reports by default.  ``--write`` applies codespell's fix wherever it
has exactly one suggestion and leaves the rest flagged; run it only
after reading every finding, since a flagged word can be an identifier
that other code, a format or a downstream caller spells the same way
(those belong in the words file), and review the diff.

Exits 0 when nothing is (or remains) flagged, 1 when something is, 2 for
a bad path or a codespell failure.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from . import gitutil
from .gitutil import ToolError
from .retoken import VENDORED

#: What `dktools spell` reads by default (git pathspecs; `*` spans
#: directories)
SPELL_PATHSPECS = (
    "src/*.c",
    "src/*.h",
    "*CMakeLists.txt",
    "docs/*.md",
    "README.md",
    "tools/src/*.py",
    "tools/tests/*.py",
    "tools/README.md",
)

#: The repository's accepted words, one per line
WORDS = "tools/spell-words.txt"

#: codespell's exit status when it flagged something (EX_DATAERR)
CODESPELL_FLAGGED = 65


def _wanted(path: str) -> bool:
    return not path.startswith(VENDORED) and not path.startswith("deps/")


def _walk(root: Path) -> list[str]:
    """The SPELL_PATHSPECS files under 'root' (a tree exported for a gate,
    which is not a git checkout)."""
    found: set[str] = set()
    for spec in SPELL_PATHSPECS:
        head, _, name = spec.rpartition("/")
        base = root / head if head else root
        # a git pathspec's '*' spans directories: rglob from its directory
        matches = base.rglob(name) if "*" in name else [base / name]
        found.update(p.relative_to(root).as_posix() for p in matches if p.is_file())
    return sorted(found)


def targets(repo: Path) -> list[str]:
    """The files `dktools spell` reads by default: the tracked ones in a
    git checkout, every matching file in an exported tree."""
    if (repo / ".git").exists():
        listed = gitutil.split_z(
            gitutil.git(repo, "ls-files", "-z", "--", *SPELL_PATHSPECS)
        )
    else:
        listed = _walk(repo)
    return [p for p in listed if _wanted(p)]


def command(repo: Path, paths: list[str], write: bool = False) -> list[str]:
    """The codespell command line for 'paths' (repo-relative)."""
    cmd = [sys.executable, "-m", "codespell_lib"]
    words = repo / WORDS
    if words.is_file():
        cmd += ["--ignore-words", str(words)]
    if write:
        cmd.append("--write-changes")
    return [*cmd, "--", *paths]


def flagged(output: str) -> list[str]:
    """The finding lines of codespell's report ('path:line: wrong ==> right')."""
    return [line for line in output.splitlines() if " ==> " in line]


def scan(repo: Path, paths: list[str], write: bool = False) -> list[str]:
    """codespell's findings over 'paths', after fixing them when 'write'."""
    proc = subprocess.run(
        command(repo, paths, write), cwd=repo, capture_output=True, text=True
    )
    if proc.returncode not in (0, CODESPELL_FLAGGED):
        raise ToolError(f"codespell failed ({proc.returncode}): {proc.stderr.strip()}")
    return flagged(proc.stdout)


def run(repo: Path, paths: tuple[str, ...], write: bool = False) -> int:
    chosen = list(paths) or targets(repo)
    for path in chosen:
        if not (repo / path).is_file():
            raise ToolError(f"{path}: no such file")
    if not chosen:
        print("spell: no files")
        return 0
    found = scan(repo, chosen)
    if write and found:
        before = len(found)
        scan(repo, chosen, write=True)
        found = scan(repo, chosen)
        print(f"spell: fixed {before - len(found)}")
    for line in found:
        print(f"  {line}")
    files = {line.split(":", 1)[0] for line in found}
    print(f"spell: {len(found)} flagged in {len(files)} of {len(chosen)} files")
    return 1 if found else 0

"""``dktools commit``: commit exactly what a passing verify built.

Usage::

    dktools commit NAME -F MESSAGE PATH... [--root DIR]

Refuses (exit 1) unless ``ROOT/verify/NAME`` holds a passing result and
each PATH's working-tree content (or its absence) is byte-for-byte what
that verify exported and built, so a commit is the verified code and
nothing edited since.  Also refuses while the index holds anything
staged, so the commit is exactly the named PATHs.  It stages them
(added, changed or deleted), runs the leakage gate on the staged diff
(a hard hit unstages these PATHs and commits nothing), and commits with
MESSAGE, which must carry the "What:", "Why:" and "How:" paragraphs.

Exits 0 on a commit, 1 on a refusal, 2 on an input error.
"""

from __future__ import annotations

import re
from pathlib import Path

from . import gitutil, leakcheck
from .gitutil import ToolError

LABELS = ("What:", "Why:", "How:")


def _passed(result: Path, name: str) -> bool:
    lines = result.read_text().splitlines()
    return bool(lines) and lines[0] == f"verify {name}: PASS"


def run(
    repo: Path, name: str, message: Path, paths: list[str], root: Path | None = None
) -> int:
    name = gitutil.check_name(name)
    if not paths:
        raise ToolError("commit needs the PATHs to commit")
    text = message.read_text()
    missing = [label for label in LABELS if not re.search(rf"(?m)^{label}", text)]
    if missing:
        raise ToolError(f"{message}: the message lacks {', '.join(missing)}")
    work = gitutil.scratch_root(root, repo).resolve() / "verify" / name
    result = work / "result.txt"
    if not result.is_file():
        raise ToolError(f"no verify result for {name} ({result})")
    rels = [gitutil.repo_path(p) for p in paths]

    if not _passed(result, name):
        print(f"commit: verify {name} did not pass ({result})")
        return 1
    staged = gitutil.git_text(repo, "diff", "--cached", "--name-only")
    if staged:
        print(f"commit: already staged: {' '.join(staged.splitlines())}")
        return 1
    differs = []
    for rel in rels:
        here, built = repo / rel, work / "src" / rel
        if here.exists() != built.exists() or (
            here.exists() and here.read_bytes() != built.read_bytes()
        ):
            differs.append(rel)
    if differs:
        for rel in differs:
            print(f"commit: {rel} differs from what verify {name} built")
        return 1

    gitutil.git(repo, "add", "--all", "--", *rels)
    hits, scanned = leakcheck.check(repo, "staged")
    if leakcheck.report(hits, scanned):
        gitutil.git(repo, "reset", "-q", "--", *rels)  # unstage these paths only
        return 1
    gitutil.git(repo, "commit", "-q", "-F", str(message))
    subject = gitutil.git_text(repo, "log", "-1", "--format=%s")
    print(f"commit: {subject} ({len(rels)} path(s), as verify {name} built them)")
    return 0

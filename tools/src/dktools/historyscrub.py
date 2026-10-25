"""``dktools historyscrub``: remove text from every version of files in history.

The edits come from one commit (``--from-commit REV``): each hunk that
commit makes to a file becomes a literal replacement (the hunk's old
lines become its new lines), applied to every version of that file in
every ref.  The commit itself then makes no change, and the rewrite
drops it, so history reads as if the text had never been there.

``check`` changes nothing.  Per file it reports:
- how many versions history holds and how many the edits change;
- every version that still matches a ``--forbid`` pattern after the
  edits (a variant of the text the edits miss);
- any other path where a hunk's old text appears (a copy elsewhere).

It exits 1 when anything remains.

``rewrite`` first runs the same check, and refuses when anything remains
or tracked files have changes.  It then:
- bundles every ref into ``build/dktools/historyscrub/NAME/backup.bundle``,
  the one copy of the old history;
- rewrites every ref with git filter-repo (a locked dependency of
  tools/, never a global install), which also rewrites old hashes
  quoted in commit messages;
- verifies that the check is clean, every ref still exists, only the
  commits the edits emptied were dropped, and HEAD's tree is unchanged
  when HEAD already held the edits;
- expires reflogs and prunes, so the old objects leave the repository.

Remotes and their configuration stay; nothing is pushed.  filter-repo's
old-to-new commit map is kept in the work directory; ``map OLD...``
translates hashes through it.
"""

from __future__ import annotations

import re
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import buildstore, gitutil, procs
from .gitutil import ToolError

#: The rewrite's wall-clock limit, in seconds.
REWRITE_TIMEOUT = 2 * 3600
ZERO = "0" * 40


@dataclass(frozen=True)
class Edit:
    """One literal replacement in one file."""

    path: str
    old: bytes
    new: bytes


def _hunks(diff: bytes) -> list[Edit]:
    """The edits of a ``git diff -U0`` (one file per ``+++`` header)."""
    edits: list[Edit] = []
    path = None
    old: list[bytes] = []
    new: list[bytes] = []

    def flush() -> None:
        if path is not None and (old or new):
            edits.append(Edit(path, b"".join(old), b"".join(new)))
        old.clear()
        new.clear()

    for line in diff.splitlines(keepends=True):
        if line.startswith(b"+++ "):
            flush()
            name = line[4:].rstrip(b"\n")
            path = name[2:].decode() if name.startswith(b"b/") else None
        elif line.startswith(b"@@"):
            flush()
        elif line.startswith(b"-") and not line.startswith(b"--- "):
            old.append(line[1:])
        elif line.startswith(b"+"):
            new.append(line[1:])
        elif line.startswith(b"\\"):
            raise ToolError("a hunk touches a file's final newline; not supported")
    flush()
    return edits


def edits_from_commit(repo: Path, rev: str) -> list[Edit]:
    """Each hunk of `rev` (against its first parent) as an Edit.  Only
    modified files and hunks that remove or replace lines qualify: an
    added line has no old text to find in older versions."""
    commit = gitutil.resolve_commit(repo, rev)
    status = gitutil.git_text(
        repo,
        "diff-tree",
        "--no-commit-id",
        "-r",
        "--no-renames",
        "--name-status",
        f"{commit}^",
        commit,
    )
    for line in status.splitlines():
        kind, _, path = line.partition("\t")
        if kind != "M":
            raise ToolError(f"{rev} {kind} {path}: only modified files can be scrubbed")
    diff = gitutil.git(
        repo,
        "diff",
        "-U0",
        "--no-color",
        "--no-renames",
        "--no-ext-diff",
        f"{commit}^",
        commit,
    )
    edits = _hunks(diff)
    for edit in edits:
        if not edit.old:
            raise ToolError(f"{rev} only adds lines to {edit.path}: nothing to remove")
    if not edits:
        raise ToolError(f"{rev} changes no lines")
    return edits


def _by_path(edits: list[Edit]) -> dict[str, list[tuple[bytes, bytes]]]:
    out: dict[str, list[tuple[bytes, bytes]]] = {}
    for edit in edits:
        out.setdefault(edit.path, []).append((edit.old, edit.new))
    return out


def apply(contents: bytes, pairs: list[tuple[bytes, bytes]]) -> bytes:
    for old, new in pairs:
        contents = contents.replace(old, new)
    return contents


def versions(repo: Path, path: str) -> list[str]:
    """Every blob `path` has had in any ref."""
    raw = gitutil.git(
        repo,
        "log",
        "--all",
        "--no-renames",
        "--format=",
        "--raw",
        "--no-abbrev",
        "--",
        path,
    )
    found = set()
    for line in raw.decode().splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[3] != ZERO:
            found.add(fields[3])
    return sorted(found)


def paths_holding(repo: Path, text: bytes) -> set[str]:
    """Paths in any ref whose count of `text` changes in some commit."""
    out = gitutil.git(
        repo,
        "log",
        "--all",
        "--no-renames",
        "--format=",
        "--name-only",
        "-S" + text.decode("utf-8", "surrogateescape"),
    )
    return {p for p in out.decode().splitlines() if p}


@dataclass
class PathScan:
    path: str
    versions: int = 0
    changed: int = 0
    #: versions still matching a forbid pattern after the edits
    remaining: list[str] = field(default_factory=list)


@dataclass
class Scan:
    paths: list[PathScan]
    #: edit path -> other paths where one of its old texts appears
    copies: dict[str, list[str]]

    @property
    def clean(self) -> bool:
        return not self.copies and not any(p.remaining for p in self.paths)

    def lines(self) -> list[str]:
        out = []
        for p in self.paths:
            out.append(
                f"{p.path}: {p.versions} versions, {p.changed} changed by the edits, "
                f"{len(p.remaining)} still forbidden"
            )
            out += [f"  still forbidden: {blob}" for blob in p.remaining]
        for path, others in self.copies.items():
            out.append(f"{path}: its old text also appears in {' '.join(others)}")
        return out


def scan(
    repo: Path, edits: list[Edit], forbid: list[str], others_too: bool = False
) -> Scan:
    """What the edits do to history; with `others_too`, a path holding
    an old text counts as a copy even when it is the edit's own path
    (after a rewrite no path may hold one)."""
    patterns = [re.compile(p.encode()) for p in forbid]
    results = []
    copies: dict[str, list[str]] = {}
    for path, pairs in _by_path(edits).items():
        blobs = versions(repo, path)
        contents = gitutil.cat_blobs(repo, blobs)
        result = PathScan(path, len(blobs))
        for blob in blobs:
            after = apply(contents[blob], pairs)
            if after != contents[blob]:
                result.changed += 1
            if any(p.search(after) for p in patterns):
                result.remaining.append(blob)
        results.append(result)
        holders: set[str] = set()
        for old, _ in pairs:
            holders |= paths_holding(repo, old)
        if not others_too:
            holders.discard(path)
        if holders:
            copies[path] = sorted(holders)
    return Scan(results, copies)


def _callback_body(edits: list[Edit]) -> str:
    """The --file-info-callback body: the edits, applied to the files
    they name (literal bytes, written with repr)."""
    table = {path.encode(): pairs for path, pairs in _by_path(edits).items()}
    return "\n".join(
        [
            f"edits = {table!r}",
            "pairs = edits.get(filename)",
            "if pairs is None:",
            "    return (filename, mode, blob_id)",
            "old = value.get_contents_by_identifier(blob_id)",
            "new = old",
            "for before, after in pairs:",
            "    new = new.replace(before, after)",
            "if new == old:",
            "    return (filename, mode, blob_id)",
            "return (filename, mode, value.insert_file_with_contents(new))",
        ]
    )


def _refs(repo: Path) -> dict[str, str]:
    """Every ref that is not symbolic, with its object."""
    out = gitutil.git_text(
        repo, "for-each-ref", "--format=%(refname) %(objectname) %(symref)"
    )
    refs = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) == 2:
            refs[fields[0]] = fields[1]
    return refs


def _commit_map(path: Path) -> dict[str, str]:
    lines = path.read_text().splitlines()[1:]  # header: "old new"
    return dict(line.split() for line in lines if line.strip())


@dataclass
class RewriteResult:
    work: Path
    bundle: Path
    rewritten: int
    dropped: list[str]
    old_head: str
    new_head: str


def rewrite(
    repo: Path, scratch: Path, name: str, edits: list[Edit], forbid: list[str]
) -> RewriteResult:
    before = scan(repo, edits, forbid)
    if not before.clean:
        raise ToolError("check is not clean:\n" + "\n".join(before.lines()))
    dirty = gitutil.git_text(repo, "status", "--porcelain", "--untracked-files=no")
    if dirty:
        raise ToolError("tracked files have changes; commit or move them first")
    metadata = repo / ".git" / "filter-repo"
    if metadata.exists():
        raise ToolError(f"{metadata} exists (a previous run's state); move it away")

    refs = _refs(repo)
    old_head = gitutil.git_text(repo, "rev-parse", "HEAD")
    old_tree = gitutil.git_text(repo, "rev-parse", "HEAD^{tree}")
    pairs = _by_path(edits)
    head_held_edits = all(
        apply(gitutil.git(repo, "show", f"HEAD:{path}"), ps)
        == gitutil.git(repo, "show", f"HEAD:{path}")
        for path, ps in pairs.items()
    )

    work = buildstore.fresh_build(
        repo, scratch, "historyscrub", name, old_head, "historyscrub"
    )
    bundle = work / "backup.bundle"
    gitutil.git(repo, "bundle", "create", str(bundle), "--all", timeout=REWRITE_TIMEOUT)
    callback = work / "callback.py"
    callback.write_text(_callback_body(edits) + "\n")
    cmd = [
        sys.executable,
        "-m",
        "git_filter_repo",
        "--force",
        "--quiet",
        "--file-info-callback",
        str(callback),
        "--refs",
        *refs,
    ]
    log = work / "filter-repo.log"
    with log.open("wb") as out:
        code = procs.run_to_file(cmd, out, cwd=repo, timeout=REWRITE_TIMEOUT)
    if code:
        raise ToolError(f"git filter-repo failed (exit {code}); see {log}")
    shutil.move(str(metadata), str(work / "filter-repo"))

    mapping = _commit_map(work / "filter-repo" / "commit-map")
    after_refs = _refs(repo)
    missing = sorted(set(refs) - set(after_refs))
    if missing:
        raise ToolError(f"refs gone after the rewrite: {' '.join(missing)}")
    dropped = sorted(old for old, new in mapping.items() if new == ZERO)
    for old in dropped:
        if (
            gitutil.git_text(
                repo, "diff-tree", "--no-commit-id", "-r", f"{old}^", old, "--", *pairs
            )
            == ""
        ):
            raise ToolError(f"dropped {old[:10]} without an edit of its own")
        touched = gitutil.git_text(
            repo, "diff-tree", "--no-commit-id", "-r", "--name-only", f"{old}^", old
        ).splitlines()
        if set(touched) - set(pairs):
            raise ToolError(f"dropped {old[:10]}, which also changed other files")
    new_head = gitutil.git_text(repo, "rev-parse", "HEAD")
    if head_held_edits:
        new_tree = gitutil.git_text(repo, "rev-parse", "HEAD^{tree}")
        if new_tree != old_tree:
            raise ToolError(f"HEAD's tree changed ({old_tree[:10]} -> {new_tree[:10]})")
    after = scan(repo, edits, forbid, others_too=True)
    if not after.clean:
        raise ToolError("history still holds the text:\n" + "\n".join(after.lines()))

    gitutil.git(repo, "reflog", "expire", "--expire=now", "--all")
    gitutil.git(repo, "gc", "--prune=now", "--quiet", timeout=REWRITE_TIMEOUT)
    rewritten = sum(1 for old, new in mapping.items() if new not in (old, ZERO))
    return RewriteResult(work, bundle, rewritten, dropped, old_head, new_head)


def translate(work: Path, hashes: list[str]) -> list[tuple[str, str]]:
    """Old commit hashes (any unique prefix) through a run's commit map."""
    mapping = _commit_map(work / "filter-repo" / "commit-map")
    out = []
    for prefix in hashes:
        hits = [old for old in mapping if old.startswith(prefix.lower())]
        if len(hits) != 1:
            raise ToolError(f"{prefix}: {len(hits)} commits in the map match")
        out.append((prefix, mapping[hits[0]]))
    return out

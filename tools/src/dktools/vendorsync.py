"""Mirror a vendored dependency from its upstream repository.

Usage::

    dktools vendorsync NAME --upstream DIR [--rev REV] [--stage]
        [--accept-local-loss] [--force-ignored] [--dry-run]

Makes ``VENDORED[NAME]`` (``varint`` -> ``deps/varint``) hold exactly
upstream's tracked files at REV (default HEAD): new files are added,
changed ones rewritten (content and executable bit), and files datakit
tracks there that upstream no longer has are deleted.  Upstream's file
list and blobs come from ``git ls-tree`` and ``git cat-file`` rather
than ``git archive``, so upstream's export attributes cannot alter the
content.  Running it again at the same REV changes nothing.

Before writing, every datakit version the sync replaces (the HEAD
blob, the index blob and the working-tree content of each changed or
deleted path) is looked up in upstream's full history.  A version
upstream never had is a datakit-local edit the sync would lose: it is
reported with its diff against upstream's REV, and the sync refuses
(exit 1) unless ``--accept-local-loss`` is given.

Upstream files that datakit's ``.gitignore`` hides (``git
check-ignore``) are written but listed separately, because ``git add``
skips them silently; ``--stage --force-ignored`` stages them with
``git add -f``.  ``--stage`` stages exactly the paths whose index
entry differs from upstream (explicit paths, never a directory);
without it the files are written and listed.  ``--dry-run`` writes and
stages nothing.

Exit codes: 0 synced, 1 refused (local edits), 2 input error.
"""

from __future__ import annotations

import difflib
import hashlib
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .gitutil import (
    ToolError,
    cat_blobs,
    git,
    git_text,
    ls_tree,
    resolve_commit,
    split_z,
)

#: Supported vendored dependencies: name -> repository-relative dir.
VENDORED = {"varint": "deps/varint"}

_MODES = {"100644", "100755", "120000"}


@dataclass(frozen=True)
class Entry:
    mode: str
    oid: str


@dataclass
class LocalVersion:
    """A datakit version of a vendored file that upstream never had."""

    path: str
    where: str  # "HEAD", "index" or "working tree"
    oid: str
    data: bytes


@dataclass
class Plan:
    upstream: dict[str, Entry]
    blobs: dict[str, bytes]
    write_add: list[str] = field(default_factory=list)
    write_mod: list[str] = field(default_factory=list)
    write_del: list[str] = field(default_factory=list)
    stage_add: list[str] = field(default_factory=list)
    stage_mod: list[str] = field(default_factory=list)
    stage_del: list[str] = field(default_factory=list)
    ignored: list[str] = field(default_factory=list)
    extra: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)
    local: list[LocalVersion] = field(default_factory=list)


def blob_id(data: bytes, object_format: str) -> str:
    """The git blob id of `data` (sha1 or sha256 repository)."""
    h = hashlib.sha256() if object_format == "sha256" else hashlib.sha1()
    h.update(b"blob %d\0" % len(data))
    h.update(data)
    return h.hexdigest()


def _worktree(path: Path) -> tuple[str, bytes] | None:
    """(mode, content) of a working-tree file, None when absent."""
    if path.is_symlink():
        return "120000", str(path.readlink()).encode("utf-8", "surrogateescape")
    if path.is_file():
        mode = "100755" if os.access(path, os.X_OK) else "100644"
        return mode, path.read_bytes()
    return None


def _entries(listing: dict[str, tuple[str, str, str]], prefix: str) -> dict[str, Entry]:
    return {
        path[len(prefix) :]: Entry(mode, oid)
        for path, (mode, kind, oid) in listing.items()
        if kind == "blob" and path.startswith(prefix)
    }


def _index(repo: Path, prefix: str) -> dict[str, Entry]:
    out = git(repo, "ls-files", "-s", "-z", "--", prefix)
    entries: dict[str, Entry] = {}
    for record in split_z(out):
        meta, path = record.split("\t", 1)
        mode, oid, _stage = meta.split()
        entries[path[len(prefix) :]] = Entry(mode, oid)
    return entries


def plan(repo: Path, dest: str, upstream_dir: Path, commit: str) -> Plan:
    """Everything the sync would do, computed without writing."""
    fmt_up = git_text(upstream_dir, "rev-parse", "--show-object-format")
    fmt_dk = git_text(repo, "rev-parse", "--show-object-format")
    if fmt_up != fmt_dk:
        raise ToolError(f"object formats differ (upstream {fmt_up}, datakit {fmt_dk})")
    prefix = dest.rstrip("/") + "/"

    listing = ls_tree(upstream_dir, commit)
    upstream: dict[str, Entry] = {}
    skipped: list[str] = []
    for path, (mode, kind, oid) in listing.items():
        if kind == "blob" and mode in _MODES:
            upstream[path] = Entry(mode, oid)
        else:
            skipped.append(f"{path} ({kind} {mode})")
    blobs = cat_blobs(upstream_dir, (e.oid for e in upstream.values()))
    result = Plan(upstream=upstream, blobs=blobs, skipped=sorted(skipped))

    index = _index(repo, prefix)
    head = _entries(ls_tree(repo, "HEAD", prefix), prefix) if _has_head(repo) else {}

    # Writes: the working tree against upstream.
    for path in sorted(upstream):
        want = upstream[path]
        have = _worktree(repo / prefix / path)
        if have is None:
            result.write_add.append(path)
        elif have != (want.mode, blobs[want.oid]):
            result.write_mod.append(path)
    for path in sorted(index):
        if path not in upstream and _worktree(repo / prefix / path) is not None:
            result.write_del.append(path)

    # Staging: the index against upstream.
    for path in sorted(upstream):
        entry = index.get(path)
        if entry is None:
            result.stage_add.append(path)
        elif entry != upstream[path]:
            result.stage_mod.append(path)
    result.stage_del = sorted(p for p in index if p not in upstream)

    # Upstream files that git add would skip.
    untracked = [prefix + p for p in sorted(upstream) if p not in index]
    if untracked:
        out = git(
            repo,
            "check-ignore",
            "-z",
            "--stdin",
            input=b"\0".join(p.encode() for p in untracked) + b"\0",
            ok_codes=(0, 1),
        )
        result.ignored = sorted(p[len(prefix) :] for p in split_z(out))

    others = git(repo, "ls-files", "-z", "--others", "--exclude-standard", "--", prefix)
    result.extra = sorted(
        p[len(prefix) :] for p in split_z(others) if p[len(prefix) :] not in upstream
    )

    result.local = _local_versions(
        repo, prefix, upstream_dir, result, head, index, fmt_dk
    )
    return result


def _has_head(repo: Path) -> bool:
    out = git(
        repo, "rev-parse", "--verify", "--quiet", "HEAD^{commit}", ok_codes=(0, 1)
    )
    return bool(out.strip())


def _local_versions(
    repo: Path,
    prefix: str,
    upstream_dir: Path,
    p: Plan,
    head: dict[str, Entry],
    index: dict[str, Entry],
    object_format: str,
) -> list[LocalVersion]:
    """Datakit versions of replaced paths that upstream never had."""
    history = {
        line.split(" ", 1)[0]
        for line in git_text(
            upstream_dir, "rev-list", "--objects", "--all"
        ).splitlines()
        if line
    }
    affected = set(p.write_mod) | set(p.write_del) | set(p.stage_mod) | set(p.stage_del)
    candidates: list[tuple[str, str, str, bytes | None]] = []
    for path in sorted(affected):
        want = p.upstream.get(path)
        for where, entries in (("HEAD", head), ("index", index)):
            entry = entries.get(path)
            if entry is not None and (want is None or entry.oid != want.oid):
                candidates.append((path, where, entry.oid, None))
        have = _worktree(repo / prefix / path)
        if have is not None and have[0] != "120000":
            oid = blob_id(have[1], object_format)
            if want is None or oid != want.oid:
                candidates.append((path, "working tree", oid, have[1]))

    missing = [c for c in candidates if c[2] not in history]
    stored = cat_blobs(repo, (oid for _, _, oid, data in missing if data is None))
    found: list[LocalVersion] = []
    seen: set[tuple[str, str]] = set()
    for path, where, oid, data in missing:
        if (path, oid) in seen:
            continue
        seen.add((path, oid))
        found.append(
            LocalVersion(path, where, oid, data if data is not None else stored[oid])
        )
    return found


def local_diff(version: LocalVersion, p: Plan, rev: str, dest: str) -> list[str]:
    """Unified diff of a local version against upstream's REV."""
    want = p.upstream.get(version.path)
    upstream_data = p.blobs[want.oid] if want is not None else b""
    if b"\0" in upstream_data or b"\0" in version.data:
        return [f"Binary file {dest}/{version.path} differs"]
    return list(
        difflib.unified_diff(
            upstream_data.decode("utf-8", "replace").splitlines(),
            version.data.decode("utf-8", "replace").splitlines(),
            fromfile=f"upstream {rev}:{version.path}"
            if want is not None
            else "/dev/null (not in upstream)",
            tofile=f"datakit {version.where}:{dest}/{version.path}",
            lineterm="",
        )
    )


def apply(repo: Path, dest: str, p: Plan) -> None:
    """Write the planned additions, modifications and deletions."""
    root = repo / dest
    for path in p.write_add + p.write_mod:
        entry = p.upstream[path]
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_symlink() or target.exists():
            target.unlink()
        data = p.blobs[entry.oid]
        if entry.mode == "120000":
            target.symlink_to(data.decode("utf-8", "surrogateescape"))
            continue
        target.write_bytes(data)
        target.chmod(0o755 if entry.mode == "100755" else 0o644)
    for path in p.write_del:
        target = root / path
        target.unlink()
        parent = target.parent
        while parent != root and parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
            parent = parent.parent


def stage(repo: Path, dest: str, p: Plan, force_ignored: bool) -> list[str]:
    """Stage the paths whose index entry differs; return them."""
    ignored = set(p.ignored)
    normal = [
        f"{dest}/{path}"
        for path in p.stage_add + p.stage_mod + p.stage_del
        if path not in ignored
    ]
    forced = [
        f"{dest}/{path}" for path in p.stage_add if path in ignored and force_ignored
    ]
    for start in range(0, len(normal), 200):
        git(repo, "add", "--", *normal[start : start + 200])
    for start in range(0, len(forced), 200):
        git(repo, "add", "-f", "--", *forced[start : start + 200])
    return normal + forced


def run(
    repo: Path,
    name: str,
    upstream_dir: Path,
    rev: str = "HEAD",
    do_stage: bool = False,
    accept_local_loss: bool = False,
    force_ignored: bool = False,
    dry_run: bool = False,
    out=None,
) -> int:
    """The CLI entry; returns the exit code (ToolError: input error)."""
    out = out or sys.stdout
    if name not in VENDORED:
        raise ToolError(
            f"unknown vendored dependency {name!r} (known: {', '.join(VENDORED)})"
        )
    if force_ignored and not do_stage:
        raise ToolError("--force-ignored only applies with --stage")
    dest = VENDORED[name]
    if not (repo / dest).is_dir():
        raise ToolError(f"{repo / dest}: not a directory")
    if not upstream_dir.is_dir():
        raise ToolError(f"{upstream_dir}: not a directory")
    commit = resolve_commit(upstream_dir, rev)
    short = commit[:7]
    subject = git_text(upstream_dir, "log", "-1", "--format=%s", commit)

    p = plan(repo, dest, upstream_dir, commit)

    print(f"vendorsync {name}: upstream {short} {subject!r} -> {dest}", file=out)
    for item in p.skipped:
        print(f"  skipped (not a file) {item}", file=out)

    if p.local:
        print(
            f"LOCAL EDITS: {len(p.local)} datakit version(s) never existed in "
            "upstream history",
            file=out,
        )
        for version in p.local:
            print(
                f"  {version.where}: {dest}/{version.path} ({version.oid[:10]})",
                file=out,
            )
            for line in local_diff(version, p, short, dest):
                print(f"    {line}", file=out)
        if not accept_local_loss:
            print("refusing to sync (pass --accept-local-loss to overwrite)", file=out)
            return 1

    written = not dry_run
    if written:
        apply(repo, dest, p)
    verb = "would write" if dry_run else "written"
    print(
        f"  working tree ({verb}): {len(p.write_add)} added, "
        f"{len(p.write_mod)} modified, {len(p.write_del)} deleted",
        file=out,
    )
    for tag, paths in (("A", p.write_add), ("M", p.write_mod), ("D", p.write_del)):
        for path in paths:
            print(f"    {tag} {dest}/{path}", file=out)

    staged: list[str] = []
    if do_stage and not dry_run:
        staged = stage(repo, dest, p, force_ignored)
    state = "staged" if staged else "not staged; --stage stages them"
    pending = len(p.stage_add) + len(p.stage_mod) + len(p.stage_del)
    if pending == 0:
        state = "nothing to stage"
    print(
        f"  vs index ({state}): {len(p.stage_add)} added, "
        f"{len(p.stage_mod)} modified, {len(p.stage_del)} deleted",
        file=out,
    )
    for tag, paths in (("A", p.stage_add), ("M", p.stage_mod), ("D", p.stage_del)):
        for path in paths:
            mark = " (ignored)" if path in p.ignored else ""
            print(f"    {tag} {dest}/{path}{mark}", file=out)

    if p.ignored:
        how = (
            "staged with git add -f"
            if staged and force_ignored
            else "git add skips these; --stage --force-ignored stages them"
        )
        print(
            f"  ignored: {len(p.ignored)} upstream file(s) hidden by .gitignore ({how})",
            file=out,
        )
        for path in p.ignored:
            print(f"    {dest}/{path}", file=out)
    else:
        print("  ignored: 0", file=out)
    if p.extra:
        print(
            f"  untracked extras (not upstream, left alone): {len(p.extra)}", file=out
        )
        for path in p.extra:
            print(f"    {dest}/{path}", file=out)
    return 0

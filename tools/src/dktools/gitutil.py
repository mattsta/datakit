"""Shared git and tree-export helpers for dktools subcommands.

Every external program runs through ``procs`` with an argument list
(never a shell), in its own process group that is killed on a
timeout or an interrupt, and every failure a user can cause (a bad
path, an unknown revision, a missing program, a git command that
hangs) surfaces as ``ToolError``, which the CLI prints as one line on
stderr with exit code 2.
"""

from __future__ import annotations

import os
import shutil
import tarfile
import tempfile
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path, PurePosixPath

from . import procs
from .procs import ToolError  # subcommands import it from here

#: Seconds any single git command may run.  Every git call dktools
#: makes is local and finishes in seconds; one still running after
#: this is stuck (and stdin is /dev/null, so it is not waiting on a
#: prompt).
GIT_TIMEOUT = 600.0


#: Environment variable naming the scratch root shared by verify and
#: validate; the --root flag overrides it.
ENV_SCRATCH = "DKTOOLS_SCRATCH"


def scratch_root(flag: Path | None, env: Mapping[str, str] | None = None) -> Path:
    """The work-dir root: --root, else $DKTOOLS_SCRATCH, else a
    ``dktools`` directory under the system temp dir."""
    if flag is not None:
        return flag
    environ = os.environ if env is None else env
    value = environ.get(ENV_SCRATCH)
    if value:
        return Path(value)
    return Path(tempfile.gettempdir()) / "dktools"


def fresh_dir(path: Path) -> Path:
    """Remove `path` if present and recreate it empty."""
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    return path


def check_name(name: str, what: str = "NAME") -> str:
    """Reject names that would escape or nest inside the scratch root."""
    if (
        not name
        or name in (".", "..")
        or "/" in name
        or "\\" in name
        or name.startswith("-")
    ):
        raise ToolError(f"{what} must be a plain directory name (got {name!r})")
    return name


def require_program(name: str) -> str:
    """Resolve `name` on PATH (or check an explicit path) or fail."""
    if os.sep in name:
        if os.access(name, os.X_OK) and Path(name).is_file():
            return name
        raise ToolError(f"{name}: not an executable file")
    found = shutil.which(name)
    if found is None:
        raise ToolError(f"{name}: not found on PATH")
    return found


def git(
    repo: Path,
    *args: str,
    env: Mapping[str, str] | None = None,
    input: bytes | None = None,
    ok_codes: Sequence[int] = (0,),
    timeout: float = GIT_TIMEOUT,
) -> bytes:
    """Run ``git -C repo ARGS`` and return stdout; ToolError on failure
    or after `timeout` seconds (the git process group is killed)."""
    full_env = None
    if env is not None:
        full_env = dict(os.environ)
        full_env.update(env)
    try:
        proc = procs.run(
            ["git", "-C", str(repo), *args],
            input=input,
            env=full_env,
            timeout=timeout,
        )
    except procs.TimedOut as exc:
        raise ToolError(f"git {args[0]}: timed out after {timeout:g}s") from exc
    if proc.returncode not in ok_codes:
        detail = proc.stderr.decode("utf-8", "replace").strip().splitlines()
        first = detail[0] if detail else f"exit {proc.returncode}"
        raise ToolError(f"git {args[0]}: {first}")
    return proc.stdout


def git_text(repo: Path, *args: str, **kwargs) -> str:
    """``git`` with stdout decoded and stripped."""
    return git(repo, *args, **kwargs).decode("utf-8", "replace").strip()


def resolve_commit(repo: Path, rev: str) -> str:
    """Full commit hash of `rev` in `repo`, or ToolError."""
    if rev.startswith("-"):
        raise ToolError(f"{rev}: not a revision")
    out = git(
        repo, "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}", ok_codes=(0, 1)
    )
    commit = out.decode().strip()
    if not commit:
        raise ToolError(f"{rev}: unknown revision in {repo}")
    return commit


def split_z(data: bytes) -> list[str]:
    """Split NUL-terminated git output into decoded fields."""
    return [f.decode("utf-8", "surrogateescape") for f in data.split(b"\0") if f]


def ls_tree(repo: Path, treeish: str, *paths: str) -> dict[str, tuple[str, str, str]]:
    """``git ls-tree -r``: path -> (mode, type, object id)."""
    out = git(repo, "ls-tree", "-r", "-z", "--full-tree", treeish, "--", *paths)
    entries: dict[str, tuple[str, str, str]] = {}
    for record in split_z(out):
        meta, path = record.split("\t", 1)
        mode, kind, oid = meta.split()
        entries[path] = (mode, kind, oid)
    return entries


def cat_blobs(repo: Path, oids: Iterable[str]) -> dict[str, bytes]:
    """Contents of the given blob ids, read in one ``cat-file --batch``."""
    wanted = list(dict.fromkeys(oids))
    if not wanted:
        return {}
    out = git(repo, "cat-file", "--batch", input=("\n".join(wanted) + "\n").encode())
    blobs: dict[str, bytes] = {}
    pos = 0
    for oid in wanted:
        nl = out.index(b"\n", pos)
        header = out[pos:nl].decode().split()
        if len(header) != 3:
            raise ToolError(f"git cat-file: {oid}: {' '.join(header)}")
        size = int(header[2])
        start = nl + 1
        blobs[oid] = out[start : start + size]
        pos = start + size + 1
    return blobs


def repo_path(spec: str) -> str:
    """Normalize a repository-relative path, rejecting escapes."""
    p = PurePosixPath(spec)
    if p.is_absolute() or not p.parts or ".." in p.parts:
        raise ToolError(f"{spec}: must be a path relative to the repository root")
    return str(p)


def build_tree(
    repo: Path,
    index_file: Path,
    specs: Sequence[tuple[str, Path | None]],
    base: str = "HEAD",
) -> str:
    """Write a tree of `base` plus exactly the given paths.

    Each spec is (PATH, FILE): FILE is None to take PATH's working-tree
    content (or its deletion, when PATH is gone from the working tree
    but present in `base`), else FILE's content is staged at PATH.  A
    private GIT_INDEX_FILE keeps the real index untouched.  Returns the
    tree object id."""
    env = {"GIT_INDEX_FILE": str(index_file)}
    git(repo, "read-tree", base, env=env)
    for path, file in specs:
        if file is not None:
            if not file.is_file():
                raise ToolError(f"{file}: not a regular file")
            oid = git_text(
                repo, "hash-object", "-w", "--path", path, "--", str(file.resolve())
            )
            mode = "100755" if os.access(file, os.X_OK) else "100644"
            git(
                repo,
                "update-index",
                "--add",
                "--cacheinfo",
                f"{mode},{oid},{path}",
                env=env,
            )
            continue
        target = repo / path
        if target.is_symlink() or target.is_file():
            git(repo, "update-index", "--add", "--", path, env=env)
        elif target.exists():
            raise ToolError(f"{path}: is a directory; list the files explicitly")
        else:
            tracked = git(repo, "ls-files", "-z", "--", path, env=env)
            if not split_z(tracked):
                raise ToolError(f"{path}: not in the working tree or {base}")
            git(repo, "update-index", "--remove", "--", path, env=env)
    return git_text(repo, "write-tree", env=env)


def index_tree(repo: Path, index_file: Path, base: str = "HEAD") -> str:
    """Write a tree of exactly the current index.

    The real index is copied to `index_file` and the tree written from
    the copy, so the real one is never touched (``git write-tree`` may
    otherwise rewrite it to cache the tree).  A repository without an
    index file yet gives `base`'s tree.  Unmerged entries are an
    error.  Returns the tree object id."""
    env = {"GIT_INDEX_FILE": str(index_file)}
    real = Path(
        git_text(repo, "rev-parse", "--path-format=absolute", "--git-path", "index")
    )
    if real.is_file():
        shutil.copyfile(real, index_file)
    else:
        git(repo, "read-tree", base, env=env)
    return git_text(repo, "write-tree", env=env)


def changed_paths(repo: Path, base: str = "HEAD") -> list[str]:
    """Every tracked path whose working-tree state differs from `base`:
    modified, added to the index, or deleted.  Untracked files are not
    included."""
    out = git(repo, "diff", "--name-only", "-z", "--no-renames", base, "--")
    return split_z(out)


def tree_changes(repo: Path, base: str, tree: str) -> list[tuple[str, str]]:
    """(status letter, path) for each path that differs from `base` to
    `tree` (A added, D deleted, M modified, T type changed)."""
    fields = split_z(
        git(repo, "diff-tree", "-r", "-z", "--no-renames", "--name-status", base, tree)
    )
    return [(fields[i], fields[i + 1]) for i in range(0, len(fields) - 1, 2)]


def export_tree(repo: Path, treeish: str, dest: Path) -> None:
    """Extract ``git archive treeish`` into `dest` with tarfile.

    git's stderr goes to a temporary file, not a second pipe: a pipe
    read only after the archive could fill and stall git while we wait
    on its stdout.  An extraction error stops git's whole process
    group; any other exception (Ctrl-C, a full disk) does too, and git
    is always reaped.  `dest` may then hold a partial tree: callers
    extract into a work dir they recreate on the next run."""
    dest.mkdir(parents=True, exist_ok=True)
    cmd = ["git", "-C", str(repo), "archive", "--format=tar", treeish]
    tar_error: tarfile.TarError | None = None
    with tempfile.TemporaryFile() as errfile:
        with procs.streaming(cmd, errfile) as proc:
            assert proc.stdout is not None
            try:
                with tarfile.open(fileobj=proc.stdout, mode="r|") as archive:
                    archive.extractall(dest, filter="data")
            except tarfile.TarError as exc:
                tar_error = exc
                procs.kill_group(proc)
        code = proc.returncode
        errfile.seek(0)
        err = errfile.read().decode("utf-8", "replace").strip()
    if err and code != 0:
        raise ToolError(f"git archive {treeish}: {err.splitlines()[0]}")
    if tar_error is not None:
        raise ToolError(f"git archive {treeish}: {tar_error}")
    if code != 0:
        raise ToolError(f"git archive {treeish}: exit {code}")

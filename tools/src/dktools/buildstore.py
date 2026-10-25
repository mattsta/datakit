"""Managed build directories: one root, an inventory, garbage collection.

Every build lives under the repository's ``build/`` directory:

  build/<preset>/                 a CMakePresets.json configure preset
  build/dktools/<kind>/<name>/    a dktools work directory (verify,
                                  validate, coverage, perfcompare)

A dktools work directory is *claimed* when a run makes it: a manifest
(``.dkbuild.json``: kind, name, revision, command, created, last used,
size) and a lock (``.dkbuild.lock``: the owning process id) are written
into it.  The lock goes when the run ends; a lock whose process is gone
reads as idle, so a crashed run never pins its directory.

``inventory`` lists every build: the managed ones, the presets, and
*legacy* ones made before this layout (``build-*`` directories at the
repository root, the top-level files of an old single ``build/`` tree,
and the scratch root dktools used in the system temp directory).

``collect`` removes idle dktools builds unused for longer than a maximum
age, then the least recently used until their total fits a byte budget.
It never removes a build in use; it removes preset builds and legacy
ones only when asked to.  Every run that makes a work directory first
collects with the default budget and age (``DKTOOLS_BUILD_BUDGET_GB``,
``DKTOOLS_BUILD_MAX_AGE_DAYS``).
"""

from __future__ import annotations

import atexit
import contextlib
import json
import os
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

MANIFEST = ".dkbuild.json"
LOCK = ".dkbuild.lock"

#: The repository-relative directory every build lives under.
BUILD_DIR = "build"

#: The dktools work directories' place under BUILD_DIR.
SCRATCH_DIR = "dktools"

ENV_BUDGET = "DKTOOLS_BUILD_BUDGET_GB"
ENV_MAX_AGE = "DKTOOLS_BUILD_MAX_AGE_DAYS"
DEFAULT_BUDGET_GB = 40.0
DEFAULT_MAX_AGE_DAYS = 14.0

#: Top-level entries of build/ that are not an old single build's files.
_PRESET_FILE = "CMakePresets.json"


def build_root(repo: Path) -> Path:
    return repo / BUILD_DIR


def default_scratch(repo: Path) -> Path:
    return build_root(repo) / SCRATCH_DIR


#: Overrides legacy_scratch (tests point it at an empty place).
ENV_LEGACY = "DKTOOLS_LEGACY_SCRATCH"


def legacy_scratch() -> Path:
    """Where dktools kept its work directories before build/dktools."""
    value = os.environ.get(ENV_LEGACY)
    return Path(value) if value else Path(tempfile.gettempdir()) / "dktools"


@dataclass
class Build:
    path: Path
    kind: str  # verify / validate / coverage / perfcompare / preset / legacy
    name: str
    managed: bool  # has a manifest
    last_used: float  # epoch seconds
    size: int | None  # bytes, None until measured
    in_use: bool
    revision: str = ""


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # alive, owned by someone else
    return True


def lock_holder(path: Path) -> int | None:
    """The live process holding `path`'s lock, or None."""
    try:
        pid = int((path / LOCK).read_text().strip() or "0")
    except (OSError, ValueError):
        return None
    return pid if _pid_alive(pid) else None


def dir_size(path: Path) -> int:
    """Bytes of the regular files under `path` (symlinks not followed)."""
    total = 0
    stack = [path]
    while stack:
        current = stack.pop()
        try:
            with os.scandir(current) as it:
                for entry in it:
                    try:
                        if entry.is_dir(follow_symlinks=False):
                            stack.append(Path(entry.path))
                        elif entry.is_file(follow_symlinks=False):
                            total += entry.stat(follow_symlinks=False).st_size
                    except OSError:
                        continue
        except OSError:
            continue
    return total


def _read_manifest(path: Path) -> dict:
    try:
        data = json.loads((path / MANIFEST).read_text())
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def _write_manifest(path: Path, data: dict) -> None:
    tmp = path / (MANIFEST + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    tmp.replace(path / MANIFEST)


class Lease:
    """A claimed build directory; `release` records its size and frees it."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.released = False

    def release(self) -> None:
        if self.released:
            return
        self.released = True
        data = _read_manifest(self.path)
        if data:
            data["lastUsed"] = time.time()
            data["size"] = dir_size(self.path)
            _write_manifest(self.path, data)
        try:
            if lock_holder(self.path) == os.getpid():
                (self.path / LOCK).unlink()
        except OSError:
            pass


def claim(
    path: Path, kind: str, name: str, revision: str = "", command: str = ""
) -> Lease:
    """Mark `path` (created if missing) as a build this process uses:
    manifest written, lock taken until `release` or process exit."""
    path.mkdir(parents=True, exist_ok=True)
    now = time.time()
    data = _read_manifest(path)
    data.update(
        {
            "kind": kind,
            "name": name,
            "revision": revision,
            "command": command,
            "lastUsed": now,
            "size": None,
        }
    )
    data.setdefault("created", now)
    _write_manifest(path, data)
    (path / LOCK).write_text(f"{os.getpid()}\n")
    lease = Lease(path)
    atexit.register(lease.release)
    return lease


def touch(path: Path) -> None:
    """Record a use of a build without claiming it (a report reread)."""
    data = _read_manifest(path)
    if data:
        data["lastUsed"] = time.time()
        _write_manifest(path, data)


def _preset_names(repo: Path) -> set[str]:
    try:
        presets = json.loads((repo / _PRESET_FILE).read_text())
    except (OSError, ValueError):
        return set()
    return {
        p.get("name", "")
        for p in presets.get("configurePresets", [])
        if not p.get("hidden")
    }


def _entry_mtime(path: Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def inventory(
    repo: Path, scratch: Path | None = None, old_scratch: Path | None = None
) -> list[Build]:
    """Every build: managed dktools ones, presets, and legacy leftovers
    (`old_scratch`, default legacy_scratch(), is the old scratch root)."""
    scratch = scratch if scratch is not None else default_scratch(repo)
    old = old_scratch if old_scratch is not None else legacy_scratch()
    builds: list[Build] = []

    # dktools work directories: build/dktools/<kind>/<name>
    if scratch.is_dir():
        for kind_dir in sorted(p for p in scratch.iterdir() if p.is_dir()):
            for path in sorted(p for p in kind_dir.iterdir() if p.is_dir()):
                data = _read_manifest(path)
                builds.append(
                    Build(
                        path=path,
                        kind=data.get("kind", kind_dir.name),
                        name=data.get("name", path.name),
                        managed=bool(data),
                        last_used=float(data.get("lastUsed") or _entry_mtime(path)),
                        size=data.get("size"),
                        in_use=lock_holder(path) is not None,
                        revision=data.get("revision", ""),
                    )
                )

    # Presets: build/<preset>
    root = build_root(repo)
    presets = _preset_names(repo)
    for name in sorted(presets):
        path = root / name
        if path.is_dir():
            builds.append(
                Build(
                    path=path,
                    kind="preset",
                    name=name,
                    managed=False,
                    last_used=_entry_mtime(path),
                    size=None,
                    in_use=lock_holder(path) is not None,
                )
            )

    # Legacy: build-* at the root, an old single build's top-level files
    # under build/, and the old temp-dir scratch root
    for path in sorted(repo.glob("build-*")):
        if path.is_dir():
            builds.append(_legacy(path, path.name))
    if root.is_dir():
        keep = presets | {SCRATCH_DIR}
        for path in sorted(root.iterdir()):
            # hidden entries (an editor's index, local settings) are not
            # an old build's output
            if path.name not in keep and not path.name.startswith("."):
                builds.append(_legacy(path, f"{BUILD_DIR}/{path.name}"))
    if old.is_dir() and old.resolve() != scratch.resolve():
        builds.append(_legacy(old, str(old)))
    return builds


def _legacy(path: Path, name: str) -> Build:
    return Build(
        path=path,
        kind="legacy",
        name=name,
        managed=False,
        last_used=_entry_mtime(path),
        size=None,
        in_use=False,
    )


def measure(builds: list[Build]) -> None:
    """Fill in the sizes not yet recorded."""
    for b in builds:
        if b.size is None:
            b.size = dir_size(b.path) if b.path.is_dir() else _file_size(b.path)


def _file_size(path: Path) -> int:
    try:
        return path.stat(follow_symlinks=False).st_size
    except OSError:
        return 0


def _remove(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path, ignore_errors=True)
    else:
        with contextlib.suppress(OSError):
            path.unlink()


@dataclass
class Policy:
    budget_bytes: int
    max_age_seconds: float
    legacy: bool = False
    presets: bool = False


def default_policy(env: dict | None = None) -> Policy:
    environ = os.environ if env is None else env
    budget = float(environ.get(ENV_BUDGET) or DEFAULT_BUDGET_GB)
    age = float(environ.get(ENV_MAX_AGE) or DEFAULT_MAX_AGE_DAYS)
    return Policy(int(budget * (1 << 30)), age * 86400.0)


def plan(builds: list[Build], policy: Policy, now: float | None = None) -> list[Build]:
    """The builds `policy` removes: idle dktools builds past the maximum
    age, then the least recently used until the rest fit the budget;
    preset and legacy builds only when the policy names them."""
    now = time.time() if now is None else now
    doomed: list[Build] = []
    work = [b for b in builds if b.kind not in ("preset", "legacy") and not b.in_use]
    for b in work:
        if now - b.last_used > policy.max_age_seconds:
            doomed.append(b)
    kept = sorted((b for b in work if b not in doomed), key=lambda b: b.last_used)
    in_use = sum(b.size or 0 for b in builds if b.in_use and b.kind != "legacy")
    total = in_use + sum(b.size or 0 for b in kept)
    for b in kept:
        if total <= policy.budget_bytes:
            break
        doomed.append(b)
        total -= b.size or 0
    if policy.presets:
        doomed += [b for b in builds if b.kind == "preset" and not b.in_use]
    if policy.legacy:
        doomed += [b for b in builds if b.kind == "legacy"]
    return doomed


def collect(builds: list[Build], policy: Policy, dry_run: bool = False) -> list[Build]:
    """Remove what `plan` picks (sizes measured first, only of the builds
    the policy can reach); returns them."""
    measure(
        [
            b
            for b in builds
            if b.kind not in ("preset", "legacy")
            or (policy.presets and b.kind == "preset")
            or (policy.legacy and b.kind == "legacy")
        ]
    )
    doomed = plan(builds, policy)
    if not dry_run:
        for b in doomed:
            _remove(b.path)
    return doomed


def human(size: int | None) -> str:
    if size is None:
        return "?"
    value = float(size)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if value < 1024 or unit == "TB":
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{value:.1f} TB"


def fresh_build(
    repo: Path,
    scratch: Path,
    kind: str,
    name: str,
    revision: str = "",
    command: str = "",
    keep: bool = False,
) -> Path:
    """The work directory scratch/kind/name, made empty (unless `keep`)
    and claimed, after collecting the other idle dktools builds under the
    default policy."""
    path = scratch / kind / name
    others = [
        b for b in inventory(repo, scratch) if b.path != path and b.kind != "legacy"
    ]
    removed = collect(others, default_policy())
    if removed:
        freed = sum(b.size or 0 for b in removed)
        print(
            f"dktools builds: collected {len(removed)} idle build(s), {human(freed)}",
            file=sys.stderr,
        )
    if not keep and path.exists():
        _remove(path)
    path.mkdir(parents=True, exist_ok=True)
    claim(path, kind, name, revision, command)
    return path


__all__ = [
    "BUILD_DIR",
    "LOCK",
    "MANIFEST",
    "Build",
    "Lease",
    "Policy",
    "build_root",
    "claim",
    "collect",
    "default_policy",
    "default_scratch",
    "dir_size",
    "fresh_build",
    "human",
    "inventory",
    "legacy_scratch",
    "lock_holder",
    "measure",
    "plan",
    "touch",
]

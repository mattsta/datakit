"""Read-only views of datakit's persisted files.

Every function opens what it reads through read-only stores and closes it
again: no call changes a file, and calls share nothing, so any number of
threads may use them.  The decoding and every check are datakit's own (C,
tested there); this module gives the results their types.

Status and kind names are datakit's own (``persistOpenResultName``,
``persistWALStopName``, ``persistBadEntryKindName``, ...), in capitals.
"""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from pathlib import Path as FsPath

from . import _datakit

type Path = str | bytes | os.PathLike[str] | os.PathLike[bytes]


class OpenError(OSError):
    """The files do not open as one structure's snapshot and WAL.

    ``reason`` names the check that refused: ``NO_PATH``, ``NO_FILE``,
    ``NOT_PERSIST``, ``WRONG_KIND``, ``UNKNOWN_TYPE`` or ``TYPES_DIFFER``.
    """

    def __init__(self, reason: str, snapshot: Path | None, wal: Path | None) -> None:
        super().__init__(f"{reason}: snapshot {snapshot!r}, WAL {wal!r}")
        self.reason = reason


@dataclass(frozen=True, slots=True)
class PersistedType:
    """One type this build persists."""

    type_id: int
    name: str
    format_version: int
    min_read_version: int


@dataclass(frozen=True, slots=True)
class FileInfo:
    """What a snapshot or WAL header says, read without the body.

    ``container_check`` / ``format_check`` say whether this build reads
    the file framing and the structure format (``OK``, or why not);
    ``readable`` is both together.
    """

    kind: str  # "snapshot" or "wal"
    file_bytes: int
    header_bytes: int
    container_version: int
    container_features: int
    container_check: str
    type_id: int
    type_name: str
    format_version: int
    format_features: int
    format_check: str
    key_id: int
    count: int  # snapshot: elements; WAL: 0
    body_bytes: int  # snapshot: body bytes; WAL: 0
    sequence: int  # snapshot: WAL sequence included; WAL: first entry's
    readable: bool


@dataclass(frozen=True, slots=True)
class Verdict:
    """Whether every envelope checks: None for a file not given."""

    snapshot_ok: bool | None
    wal_ok: bool | None
    error: str  # persistLastError's name; "NONE" when it set none

    @property
    def ok(self) -> bool:
        return self.snapshot_ok is not False and self.wal_ok is not False


@dataclass(frozen=True, slots=True)
class WalEntry:
    """One WAL entry: its op's body is shown as bytes (its layout is the
    type's own)."""

    sequence: int
    offset: int
    op: int
    op_name: str
    body: bytes


@dataclass(frozen=True, slots=True)
class WalWalk:
    """A WAL's entries, and where and why the walk stopped.

    ``stop`` is ``END``, ``CALLBACK`` (the limit was reached), ``TORN``
    (the WAL ends inside an entry), ``BAD`` (``bad_kind``: ``CORRUPT``,
    ``KEY`` or ``READ``) or ``UNREADABLE``.  ``good_end`` is the end of
    the last complete entry: where a heal would cut, with
    ``wal_end - good_end`` bytes past it.
    """

    entries: list[WalEntry]
    stop: str
    bad_kind: str
    good_end: int
    wal_end: int


@dataclass(frozen=True, slots=True)
class RecoverSummary:
    """What recovery rebuilds, built and freed without keeping it."""

    recovered: bool
    valid: bool
    count: int
    bytes: int
    entries_recovered: int
    entries_skipped: int


@dataclass(frozen=True, slots=True)
class QuarantineRecord:
    """One record of a quarantine file: WAL bytes set aside by a heal."""

    at: int  # the record's own offset in the quarantine file
    wal_offset: int  # where the bad entry sat in its WAL
    next_sequence: int  # the WAL's next sequence when it was cut
    wal_bytes: bytes


@dataclass(frozen=True, slots=True)
class Quarantine:
    """A quarantine file's records; ``end`` is ``END`` or ``DAMAGED``
    (bytes after the last record that are not a whole record)."""

    records: list[QuarantineRecord]
    end: str
    file_bytes: int


@dataclass(frozen=True, slots=True)
class RepairPlan:
    """What a repair (an open) would do to a snapshot and WAL.

    ``action``: ``NONE`` (the WAL is whole), ``CUT`` (a torn tail: the
    WAL is cut at ``cut_at``), ``HEAL`` (a corrupt entry: it and the
    ``bytes_past`` bytes after it move into the quarantine and the WAL is
    cut at ``cut_at``) or ``HOLD`` (a key or read problem, or a WAL that
    does not read: nothing changes).  ``rebuilt`` is what the files
    rebuild afterwards: the entries before the stop.
    """

    action: str
    stop: str
    bad_kind: str
    cut_at: int
    bytes_past: int
    entries_kept: int
    rebuilt: RecoverSummary


@dataclass(frozen=True, slots=True)
class RepairResult:
    """A repair done on copies in a new directory.

    ``bad_*`` describe the bad entry met (``bad_kind`` ``NONE``: none; a
    torn tail is not one), ``bad_action`` ``HEALED`` or ``HELD``.
    ``quarantine`` is the quarantine file when one exists afterwards.
    """

    snapshot: FsPath
    wal: FsPath
    quarantine: FsPath | None
    synced: bool
    bad_kind: str
    bad_action: str
    bad_offset: int
    bad_bytes: int
    entries_before: int
    recovered: bool
    valid: bool
    count: int
    bytes: int


def quarantine_name(wal: FsPath) -> str:
    """The quarantine file's name beside a WAL: ``<base>.quarantine`` for
    ``<base>.wal``, as a persist context names it."""
    name = wal.name
    base = name.removesuffix(".wal")
    return f"{base}.quarantine"


def repair_plan(snapshot: Path, wal: Path) -> RepairPlan:
    """What a repair would do, read without changing anything."""
    walk = wal_walk(wal, snapshot)
    if walk.stop == "END":
        action = "NONE"
    elif walk.stop == "TORN":
        action = "CUT"
    elif walk.stop == "BAD" and walk.bad_kind == "CORRUPT":
        action = "HEAL"
    else:
        action = "HOLD"
    return RepairPlan(
        action=action,
        stop=walk.stop,
        bad_kind=walk.bad_kind,
        cut_at=walk.good_end,
        bytes_past=walk.wal_end - walk.good_end,
        entries_kept=len(walk.entries),
        rebuilt=recover(snapshot, wal),
    )


def repair(snapshot: Path, wal: Path, out_dir: Path) -> RepairResult:
    """Repair copies of the snapshot and WAL in ``out_dir``, as an open
    does; the originals are never opened for writing.

    ``out_dir`` is created if needed; it may not be the directory of
    either original (ValueError) or already hold a file the repair would
    write (FileExistsError).  A quarantine file beside the original WAL is
    copied too, so new records follow the old ones.
    """
    snapshot, wal, out = (FsPath(os.fsdecode(p)) for p in (snapshot, wal, out_dir))
    if out.exists() and any(
        out.resolve() == p.resolve().parent for p in (snapshot, wal)
    ):
        raise ValueError(f"{out}: repairs go to a new directory, not the originals'")
    quarantine = wal.parent / quarantine_name(wal)
    targets = [out / snapshot.name, out / wal.name, out / quarantine.name]
    if len({t.name for t in targets}) != len(targets):
        raise ValueError("the snapshot, WAL and quarantine need different names")
    for target in targets:
        if target.exists():
            raise FileExistsError(f"{target}: already there")
    # Opening reads the headers and refuses before anything is copied
    verify(snapshot, wal)
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(snapshot, targets[0])
    shutil.copyfile(wal, targets[1])
    if quarantine.exists():
        shutil.copyfile(quarantine, targets[2])
    status, report = _datakit.repair(targets[0], targets[1], targets[2])
    if status != "OK":
        raise OpenError(status, targets[0], targets[1])
    return RepairResult(
        snapshot=targets[0],
        wal=targets[1],
        quarantine=targets[2] if targets[2].exists() else None,
        **report,
    )


def registry() -> list[PersistedType]:
    """Every type this build persists."""
    return [PersistedType(*row) for row in _datakit.registry()]


def inspect(path: Path) -> FileInfo:
    """A snapshot's or WAL's header."""
    status, info = _datakit.inspect(path)
    if info is None:
        raise OpenError(status, path, None)
    return FileInfo(**info)


def verify(snapshot: Path | None = None, wal: Path | None = None) -> Verdict:
    """Check every envelope of the snapshot and the WAL given."""
    status, snapshot_ok, wal_ok, error = _datakit.verify(snapshot, wal)
    if status != "OK":
        raise OpenError(status, snapshot, wal)
    return Verdict(snapshot_ok, wal_ok, error)


def wal_walk(wal: Path, snapshot: Path | None = None, *, limit: int = 0) -> WalWalk:
    """The WAL's entries in order (the first ``limit``; 0: all) and where
    the walk stopped."""
    status, entries, report = _datakit.wal_walk(snapshot, wal, limit)
    if status != "OK":
        raise OpenError(status, snapshot, wal)
    report.pop("entries")
    return WalWalk(entries=[WalEntry(*e) for e in entries], **report)


def recover(snapshot: Path, wal: Path | None = None) -> RecoverSummary:
    """Recover through the type's own descriptor and summarize the result."""
    status, summary = _datakit.recover(snapshot, wal)
    if status != "OK":
        raise OpenError(status, snapshot, wal)
    return RecoverSummary(**summary)


def quarantine(path: Path) -> Quarantine:
    """A quarantine file's records, in order."""
    status, records, end, file_bytes = _datakit.quarantine(path)
    if status != "OK":
        raise OpenError(status, path, None)
    return Quarantine([QuarantineRecord(*r) for r in records], end, file_bytes)

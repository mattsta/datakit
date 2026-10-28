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
from dataclasses import dataclass

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

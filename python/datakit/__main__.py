"""``python -m datakit``: operator commands for datakit's persisted files.

    python -m datakit inspect FILE...
    python -m datakit verify FILE [FILE]
    python -m datakit dump WAL [SNAPSHOT] [--limit N]
    python -m datakit stats FILE [FILE]
    python -m datakit quarantine FILE [--show N]

FILE arguments are snapshots or WALs in any order: each one's header says
which it is.  Every command reads files read-only and changes nothing.
``--json`` prints JSON instead of text (``dump`` prints one JSON object
per entry, then one for the stop).  Op bodies and quarantined bytes are
shown as hex: their layout belongs to the structure's type.

Exit status: 0 when everything read is whole and checks; 1 when a check
fails or a file is damaged (a failed envelope, a WAL that stops before
its end, a damaged quarantine record); 2 when the files do not open.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from collections.abc import Sequence
from typing import Any

from . import persist

#: exit statuses
CLEAN = 0
DAMAGED = 1
UNOPENED = 2


def _pair(files: Sequence[str]) -> tuple[str | None, str | None]:
    """(snapshot, wal) from up to two files, each placed by its header."""
    if not 1 <= len(files) <= 2:
        raise persist.OpenError("NO_PATH", None, None)
    snapshot = wal = None
    for path in files:
        if persist.inspect(path).kind == "wal":
            if wal is not None:
                raise persist.OpenError("WRONG_KIND", snapshot, path)
            wal = path
        else:
            if snapshot is not None:
                raise persist.OpenError("WRONG_KIND", path, wal)
            snapshot = path
    return snapshot, wal


def _plain(value: Any) -> Any:
    """A result as JSON-ready data: dataclasses as dicts, bytes as hex."""
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return {
            f.name: _plain(getattr(value, f.name)) for f in dataclasses.fields(value)
        }
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, list):
        return [_plain(v) for v in value]
    if isinstance(value, dict):
        return {k: _plain(v) for k, v in value.items()}
    return value


def _emit(args: argparse.Namespace, value: Any, text: str) -> None:
    print(json.dumps(_plain(value), sort_keys=True) if args.json else text)


def _fields(value: Any, skip: Sequence[str] = ()) -> str:
    return "\n".join(
        f"  {f.name}: {getattr(value, f.name)}"
        for f in dataclasses.fields(value)
        if f.name not in skip
    )


def cmd_inspect(args: argparse.Namespace) -> int:
    status = CLEAN
    for path in args.files:
        info = persist.inspect(path)
        _emit(args, {"path": path, **_plain(info)}, f"{path}\n{_fields(info)}")
        if not info.readable:
            status = DAMAGED
    return status


def cmd_verify(args: argparse.Namespace) -> int:
    snapshot, wal = _pair(args.files)
    v = persist.verify(snapshot, wal)
    lines = (
        [f"snapshot {snapshot}: {'ok' if v.snapshot_ok else 'FAILED'}"]
        if snapshot
        else []
    )
    if wal:
        lines.append(f"WAL {wal}: {'ok' if v.wal_ok else 'FAILED'}")
    if v.error != "NONE":
        lines.append(f"error: {v.error}")
    _emit(args, v, "\n".join(lines))
    return CLEAN if v.ok else DAMAGED


def _stop_text(walk: persist.WalWalk) -> str:
    past = walk.wal_end - walk.good_end
    why = walk.stop if walk.stop != "BAD" else f"BAD ({walk.bad_kind})"
    return (
        f"stop: {why} after {len(walk.entries)} entries; last complete entry "
        f"ends at {walk.good_end}, {past} bytes past it"
    )


def _whole(walk: persist.WalWalk) -> bool:
    return walk.stop in ("END", "CALLBACK")


def cmd_dump(args: argparse.Namespace) -> int:
    snapshot, wal = _pair(args.files)
    if wal is None:
        raise persist.OpenError("WRONG_KIND", snapshot, None)
    walk = persist.wal_walk(wal, snapshot, limit=args.limit)
    for e in walk.entries:
        _emit(
            args,
            e,
            f"{e.sequence:>8} @{e.offset:<10} {e.op_name:<12} {e.body.hex()}",
        )
    stop = {k: v for k, v in _plain(walk).items() if k != "entries"}
    _emit(args, {"stop": stop}, _stop_text(walk))
    return CLEAN if _whole(walk) else DAMAGED


def cmd_stats(args: argparse.Namespace) -> int:
    snapshot, wal = _pair(args.files)
    status = CLEAN
    out: dict[str, Any] = {}
    text = []
    if snapshot is not None:
        summary = persist.recover(snapshot, wal)
        out["recover"] = summary
        text.append(f"recover\n{_fields(summary)}")
        if not (summary.recovered and summary.valid):
            status = DAMAGED
    if wal is not None:
        walk = persist.wal_walk(wal, snapshot)
        out["wal"] = {k: v for k, v in _plain(walk).items() if k != "entries"}
        out["wal"]["entries"] = len(walk.entries)
        text.append(f"WAL {wal}\n  {_stop_text(walk)}")
        if not _whole(walk):
            status = DAMAGED
    _emit(args, out, "\n".join(text))
    return status


def cmd_quarantine(args: argparse.Namespace) -> int:
    q = persist.quarantine(args.file)
    if args.show is not None:
        if not 0 <= args.show < len(q.records):
            print(
                f"no record {args.show} (the file holds {len(q.records)})",
                file=sys.stderr,
            )
            return UNOPENED
        r = q.records[args.show]
        _emit(
            args,
            r,
            f"{_fields(r, skip=('wal_bytes',))}\n  wal_bytes: {r.wal_bytes.hex()}",
        )
    else:
        for i, r in enumerate(q.records):
            _emit(
                args,
                {"index": i, **_plain(r)},
                f"{i:>4} @{r.at:<10} WAL offset {r.wal_offset}, next sequence "
                f"{r.next_sequence}, {len(r.wal_bytes)} bytes",
            )
        _emit(
            args,
            {"end": q.end, "records": len(q.records), "file_bytes": q.file_bytes},
            f"{len(q.records)} records, {q.end}",
        )
    return CLEAN if q.end == "END" else DAMAGED


def _count(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError(f"{value}: a count is 0 or more")
    return value


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m datakit",
        description="Read datakit's persisted files; nothing is ever written.",
    )
    p.add_argument("--json", action="store_true", help="JSON output")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("inspect", help="each file's header")
    s.add_argument("files", nargs="+")
    s.set_defaults(run=cmd_inspect)

    s = sub.add_parser("verify", help="check every envelope of a snapshot and/or WAL")
    s.add_argument("files", nargs="+", metavar="FILE")
    s.set_defaults(run=cmd_verify)

    s = sub.add_parser("dump", help="a WAL's entries and where it stops")
    s.add_argument("files", nargs="+", metavar="FILE")
    s.add_argument("--limit", type=_count, default=0, help="first N entries (0: all)")
    s.set_defaults(run=cmd_dump)

    s = sub.add_parser("stats", help="what recovery rebuilds and where the WAL stops")
    s.add_argument("files", nargs="+", metavar="FILE")
    s.set_defaults(run=cmd_stats)

    s = sub.add_parser("quarantine", help="a quarantine file's records")
    s.add_argument("file")
    s.add_argument("--show", type=_count, metavar="N", help="record N with its bytes")
    s.set_defaults(run=cmd_quarantine)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        return args.run(args)
    except persist.OpenError as exc:
        print(f"cannot open: {exc}", file=sys.stderr)
        return UNOPENED


if __name__ == "__main__":
    sys.exit(main())

# Operating on persisted files

How to look at a structure's files at rest: what they hold, whether they
check, where a WAL stops and what a heal set aside. Every command here reads
files read-only; none of them changes a file.

The tools are the Python package in `python/` (`datakit`, one wheel for
Python 3.15+, regular or free-threaded) over datakit's own calls (see
PERSIST.md, "Reading files by path (operators)"). Build and install it with
`dktools pybuild NAME`, or use the environment `dktools pytest NAME` makes.

## The files

A structure persisted through a `persistCtx` has up to three files:

- `<base>.snap`: the snapshot. Its header names the structure's type, the
  element count and the WAL sequence it includes.
- `<base>.wal`: the WAL, the operations logged since that snapshot, one
  entry each (a 4-byte length, then the entry's envelope).
- `<base>.quarantine`: present only after a heal. Each record holds WAL
  bytes that failed a check, exactly as they were, and where they came from.

## Commands

```sh
python -m datakit inspect FILE...          # each file's header
python -m datakit verify FILE [FILE]       # check every envelope
python -m datakit dump WAL [SNAPSHOT] [--limit N]
python -m datakit stats FILE [FILE]        # what recovery rebuilds
python -m datakit quarantine FILE [--show N]
```

Give a snapshot and its WAL in either order: each file's header says which
it is. `--json` (before the command) prints JSON; `dump` prints one object
per entry and a last one for the stop, so its output is JSON Lines.

| Exit | Meaning                                                                  |
| ---- | ------------------------------------------------------------------------ |
| 0    | everything read is whole and checks                                      |
| 1    | a check failed or a file is damaged (see the output for which and where) |
| 2    | the files did not open (named reason: `NO_FILE`, `NOT_PERSIST`, ...)     |

### inspect

The header without the body: kind, the type and its format stamp, the file
framing's stamp, key id, count, body bytes and sequence. `container_check`
and `format_check` say whether this build reads the file (`OK`, or the
reason: `VERSION_TOO_NEW`, `WRONG_ID`, ...; `UNKNOWN_TYPE` for a type this
build does not persist); `readable` is both. An unreadable header exits 1.

### verify

Checks every envelope of the snapshot and the WAL given. A keyed file
(written under an HMAC or keyed BLAKE3 digest) checks only under its key,
which these tools do not take: its envelopes fail here with `error`
`NO_KEY_ATTACHED`, and a walk of its WAL stops at the first entry with
`BAD` / `KEY`. That is not damage; read it where the key is bound.

### dump

Each WAL entry in order: sequence, offset in the file, op name and the op's
body as hex (the body's layout belongs to the structure's type; an intset
insert, for example, is the 8-byte little-endian member). The last line
says where the walk stopped:

| stop   | Meaning                                                             |
| ------ | ------------------------------------------------------------------- |
| `END`  | every entry read; the WAL is whole                                  |
| `TORN` | the WAL ends inside an entry: an append a crash cut short           |
| `BAD`  | an entry failed its check; `bad_kind` is `CORRUPT`, `KEY` or `READ` |

`good_end` is the end of the last complete entry: where a heal cuts the
WAL. `wal_end - good_end` bytes lie past it. With `--limit N` the walk stops
after N entries (`CALLBACK`) and that is not damage.

### stats

Recovers the structure from the snapshot (and the WAL) through its own
type, reports whether it restored, whether the type's own validation
accepts it, its element count and estimated size, and the entries replayed
and skipped, then frees it. With a WAL it also gives the WAL's stop. A torn
or bad WAL still recovers every entry before the stop, as an open does.

### quarantine

Lists the records: their offset in the quarantine file, the WAL offset the
bad entry sat at, the WAL's next sequence when it was cut and the byte
count. `--show N` prints record N with its bytes. The end is `END`, or
`DAMAGED` when bytes after the last whole record are not a record (a short
header, a wrong magic, a count past the end of the file).

## Reading a damaged WAL

- `TORN`: a crash during an append. The entries before it are whole; the
  next open cuts the tail and goes on.
- `BAD` with `CORRUPT`: damage. With a quarantine attached the next open
  heals: it moves the bad entry and everything after it into the
  quarantine and cuts the WAL at `good_end`. Without one, the WAL is held:
  it takes no appends until `persistCompact` writes a fresh one.
- `BAD` with `KEY`: the entries may be whole under another key. Nothing is
  healed; the WAL is held until it reads under the right key.
- `BAD` with `READ`: the store failed to read. Check the device; the WAL is
  held and nothing is moved.

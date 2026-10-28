"""datakit.persist against real files written through datakit's own API.

Run through ``dktools pytest NAME`` (after ``dktools pybuild NAME``): the
installed wheel, on the regular and the free-threaded interpreter.
"""

from __future__ import annotations

import struct
import sys
import sysconfig
import tempfile
import threading
import unittest
from pathlib import Path

from datakit import _datakit, persist


class Files(unittest.TestCase):
    """An intset snapshot holding {10, 20} and a WAL inserting 1, 2, 3."""

    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)
        self.snap = self.dir / "s.snap"
        self.wal = self.dir / "s.wal"
        _datakit._write_intset(self.snap, self.wal, [10, 20], [1, 2, 3])
        self.wal_bytes = self.wal.read_bytes()
        self.snap_bytes = self.snap.read_bytes()

    def tearDown(self) -> None:
        self._dir.cleanup()

    def assertUnchanged(self) -> None:
        self.assertEqual(self.snap.read_bytes(), self.snap_bytes)

    def entry_offsets(self) -> list[int]:
        return [e.offset for e in persist.wal_walk(self.wal).entries]

    def flipped_second_entry(self) -> bytes:
        """The WAL with one bit flipped mid-way through its second entry."""
        at = self.entry_offsets()[1]
        (length,) = struct.unpack_from("<I", self.wal_bytes, at)
        bad = bytearray(self.wal_bytes)
        bad[at + 4 + length // 2] ^= 0x10
        return bytes(bad)


class Registry(unittest.TestCase):
    def test_every_type_once_with_a_name_and_versions(self) -> None:
        types = persist.registry()
        ids = [t.type_id for t in types]
        self.assertEqual(len(ids), len(set(ids)))
        names = {t.type_id: t for t in types}
        self.assertEqual(names[2].name, "intset")
        for t in types:
            self.assertTrue(t.name and t.name != "unknown", t)
            self.assertGreaterEqual(t.format_version, t.min_read_version)
            self.assertGreaterEqual(t.min_read_version, 1)


class Inspect(Files):
    def test_a_snapshot_header(self) -> None:
        info = persist.inspect(self.snap)
        self.assertEqual(info.kind, "snapshot")
        self.assertEqual((info.type_id, info.type_name), (2, "intset"))
        self.assertEqual(info.count, 2)
        self.assertEqual(info.file_bytes, len(self.snap_bytes))
        self.assertEqual((info.container_check, info.format_check), ("OK", "OK"))
        self.assertTrue(info.readable)

    def test_a_wal_header(self) -> None:
        info = persist.inspect(self.wal)
        self.assertEqual(info.kind, "wal")
        self.assertEqual(info.type_name, "intset")
        self.assertEqual(info.count, 0)
        self.assertEqual(info.file_bytes, len(self.wal_bytes))

    def test_a_missing_file_and_a_foreign_one_refuse_by_name(self) -> None:
        with self.assertRaises(persist.OpenError) as missing:
            persist.inspect(self.dir / "nope")
        self.assertEqual(missing.exception.reason, "NO_FILE")
        junk = self.dir / "junk"
        junk.write_bytes(b"not a persisted file at all")
        with self.assertRaises(persist.OpenError) as foreign:
            persist.inspect(junk)
        self.assertEqual(foreign.exception.reason, "NOT_PERSIST")

    def test_paths_may_be_str_bytes_or_pathlike(self) -> None:
        for path in (self.snap, str(self.snap), bytes(self.snap)):
            with self.subTest(path=type(path)):
                self.assertEqual(persist.inspect(path).count, 2)


class Verify(Files):
    def test_good_files_verify_and_stay_as_they_were(self) -> None:
        v = persist.verify(self.snap, self.wal)
        self.assertEqual((v.snapshot_ok, v.wal_ok, v.error), (True, True, "NONE"))
        self.assertTrue(v.ok)
        self.assertEqual(persist.verify(wal=self.wal).snapshot_ok, None)
        self.assertUnchanged()
        self.assertEqual(self.wal.read_bytes(), self.wal_bytes)

    def test_a_flipped_byte_fails_the_wal_and_nothing_is_written(self) -> None:
        bad = self.flipped_second_entry()
        self.wal.write_bytes(bad)
        v = persist.verify(self.snap, self.wal)
        self.assertEqual((v.snapshot_ok, v.wal_ok), (True, False))
        self.assertFalse(v.ok)
        self.assertEqual(self.wal.read_bytes(), bad)

    def test_refusals_name_their_reason(self) -> None:
        for snapshot, wal, reason in (
            (None, None, "NO_PATH"),
            (self.dir / "nope", None, "NO_FILE"),
            (self.wal, None, "WRONG_KIND"),
            (None, self.snap, "WRONG_KIND"),
        ):
            with self.subTest(reason=reason, snapshot=snapshot, wal=wal):
                with self.assertRaises(persist.OpenError) as cm:
                    persist.verify(snapshot, wal)
                self.assertEqual(cm.exception.reason, reason)


class WalWalk(Files):
    def test_a_clean_wal_walks_to_its_end(self) -> None:
        walk = persist.wal_walk(self.wal, self.snap)
        self.assertEqual(walk.stop, "END")
        self.assertEqual(walk.bad_kind, "NONE")
        self.assertEqual([e.op_name for e in walk.entries], ["INSERT"] * 3)
        self.assertEqual(walk.good_end, len(self.wal_bytes))
        self.assertEqual(walk.wal_end, len(self.wal_bytes))
        seqs = [e.sequence for e in walk.entries]
        self.assertEqual(seqs, sorted(seqs))
        # Each entry starts where the one before ends: a 4-byte length
        # prefix, then that many bytes
        offsets = [e.offset for e in walk.entries] + [len(self.wal_bytes)]
        for at, nxt in zip(offsets, offsets[1:]):
            (length,) = struct.unpack_from("<I", self.wal_bytes, at)
            self.assertEqual(at + 4 + length, nxt)
        # An intset insert's body is the member, 8 bytes little-endian
        self.assertEqual(
            [struct.unpack("<q", e.body)[0] for e in walk.entries], [1, 2, 3]
        )

    def test_a_limit_stops_the_walk_where_the_next_entry_starts(self) -> None:
        walk = persist.wal_walk(self.wal, limit=2)
        self.assertEqual((walk.stop, len(walk.entries)), ("CALLBACK", 2))
        self.assertEqual(walk.good_end, self.entry_offsets()[2])
        with self.assertRaises(ValueError):
            persist.wal_walk(self.wal, limit=-1)

    def test_a_torn_tail_and_a_flipped_byte_report_their_cut(self) -> None:
        offsets = self.entry_offsets()
        for data, stop, kind, kept in (
            (self.wal_bytes[:-3], "TORN", "NONE", 2),
            (self.flipped_second_entry(), "BAD", "CORRUPT", 1),
        ):
            with self.subTest(stop=stop):
                self.wal.write_bytes(data)
                walk = persist.wal_walk(self.wal)
                self.assertEqual((walk.stop, walk.bad_kind), (stop, kind))
                self.assertEqual(len(walk.entries), kept)
                self.assertEqual(walk.good_end, offsets[kept])
                self.assertEqual(walk.wal_end, len(data))
                summary = persist.recover(self.snap, self.wal)
                self.assertEqual(summary.count, 2 + kept)
                self.assertEqual(self.wal.read_bytes(), data)


class Recover(Files):
    def test_a_snapshot_and_its_wal_rebuild_every_member(self) -> None:
        s = persist.recover(self.snap, self.wal)
        self.assertTrue(s.recovered and s.valid)
        self.assertEqual((s.count, s.entries_recovered, s.entries_skipped), (5, 3, 0))
        self.assertGreater(s.bytes, 0)
        self.assertEqual(persist.recover(self.snap).count, 2)
        self.assertUnchanged()


class Quarantine(unittest.TestCase):
    """Quarantine files laid out as the heal path writes them."""

    @staticmethod
    def record(wal_offset: int, sequence: int, data: bytes) -> bytes:
        head = b"DKPQ\x01\x00\x00\x00" + struct.pack(
            "<qQQ", wal_offset, sequence, len(data)
        )
        return head + data

    def read(self, data: bytes) -> persist.Quarantine:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "q"
            path.write_bytes(data)
            return persist.quarantine(path)

    def test_records_read_back_in_order(self) -> None:
        a = self.record(40, 11, b"\x09\x08\x07")
        b = self.record(77, 12, b"")
        q = self.read(a + b)
        self.assertEqual(q.end, "END")
        self.assertEqual(q.file_bytes, len(a + b))
        self.assertEqual(
            [(r.at, r.wal_offset, r.next_sequence, r.wal_bytes) for r in q.records],
            [(0, 40, 11, b"\x09\x08\x07"), (len(a), 77, 12, b"")],
        )

    def test_damage_after_whole_records_reads_as_damaged(self) -> None:
        good = self.record(40, 11, b"abc")
        huge = b"DKPQ\x01\x00\x00\x00" + struct.pack("<qQQ", 1, 2, 1 << 62)
        for data, kept in (
            (good + good[:20], 1),  # a short header
            (good + b"XKPQ" + good[4:], 1),  # a wrong magic
            (huge, 0),  # a count past the end: refused, never allocated
            (good[:-1], 0),  # a body cut short
        ):
            with self.subTest(size=len(data), kept=kept):
                q = self.read(data)
                self.assertEqual((q.end, len(q.records)), ("DAMAGED", kept))

    def test_an_empty_file_has_no_records_and_a_missing_one_refuses(self) -> None:
        self.assertEqual(self.read(b"").records, [])
        with (
            tempfile.TemporaryDirectory() as d,
            self.assertRaises(persist.OpenError) as cm,
        ):
            persist.quarantine(Path(d) / "missing")
        self.assertEqual(cm.exception.reason, "NO_FILE")


class Threads(Files):
    def test_many_threads_read_the_same_files_at_once(self) -> None:
        results: list[int] = []
        lock = threading.Lock()

        def work() -> None:
            for _ in range(20):
                count = persist.recover(self.snap, self.wal).count
                with lock:
                    results.append(count)

        threads = [threading.Thread(target=work) for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(results, [5] * 160)

    def test_the_free_threaded_interpreter_keeps_the_gil_off(self) -> None:
        if not sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.skipTest("a regular (GIL) interpreter")
        self.assertFalse(sys._is_gil_enabled(), "importing datakit turned the GIL on")


if __name__ == "__main__":
    unittest.main()

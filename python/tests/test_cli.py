"""``python -m datakit``: each command on good and damaged files, text and JSON."""

from __future__ import annotations

import contextlib
import io
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from datakit import _datakit, persist
from datakit.__main__ import CLEAN, DAMAGED, UNOPENED, main


class Cli(unittest.TestCase):
    """An intset snapshot holding {10, 20} and a WAL inserting 1, 2, 3."""

    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)
        self.snap = str(self.dir / "s.snap")
        self.wal = str(self.dir / "s.wal")
        _datakit._write_intset(self.snap, self.wal, [10, 20], [1, 2, 3])
        self.wal_bytes = Path(self.wal).read_bytes()

    def tearDown(self) -> None:
        self._dir.cleanup()

    def run_cli(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def json_lines(self, *argv: str) -> tuple[int, list[dict]]:
        code, out, _ = self.run_cli("--json", *argv)
        return code, [json.loads(line) for line in out.splitlines()]

    def tear_wal(self) -> None:
        Path(self.wal).write_bytes(self.wal_bytes[:-3])

    def test_inspect_reads_each_header(self) -> None:
        code, rows = self.json_lines("inspect", self.wal, self.snap)
        self.assertEqual(code, CLEAN)
        self.assertEqual([r["kind"] for r in rows], ["wal", "snapshot"])
        self.assertEqual([r["path"] for r in rows], [self.wal, self.snap])
        self.assertEqual(rows[1]["count"], 2)
        code, out, _ = self.run_cli("inspect", self.snap)
        self.assertIn("type_name: intset", out)

    def test_verify_places_files_by_their_headers(self) -> None:
        for order in ((self.snap, self.wal), (self.wal, self.snap)):
            with self.subTest(order=order):
                code, out, _ = self.run_cli("verify", *order)
                self.assertEqual(code, CLEAN, out)
                self.assertIn(f"snapshot {self.snap}: ok", out)
                self.assertIn(f"WAL {self.wal}: ok", out)

    def test_verify_fails_a_damaged_wal(self) -> None:
        at = persist.wal_walk(self.wal).entries[1].offset
        (length,) = struct.unpack_from("<I", self.wal_bytes, at)
        bad = bytearray(self.wal_bytes)
        bad[at + 4 + length // 2] ^= 0x10
        Path(self.wal).write_bytes(bad)
        code, rows = self.json_lines("verify", self.wal)
        self.assertEqual(code, DAMAGED)
        self.assertEqual((rows[0]["wal_ok"], rows[0]["snapshot_ok"]), (False, None))

    def test_dump_lists_entries_then_the_stop(self) -> None:
        code, rows = self.json_lines("dump", self.wal)
        self.assertEqual(code, CLEAN)
        entries, stop = rows[:-1], rows[-1]["stop"]
        self.assertEqual([e["op_name"] for e in entries], ["INSERT"] * 3)
        self.assertEqual(
            [struct.unpack("<q", bytes.fromhex(e["body"]))[0] for e in entries],
            [1, 2, 3],
        )
        self.assertEqual((stop["stop"], stop["good_end"]), ("END", len(self.wal_bytes)))
        code, rows = self.json_lines("dump", self.wal, "--limit", "1")
        self.assertEqual((code, len(rows)), (CLEAN, 2))

    def test_dump_reports_a_torn_tail_as_damage(self) -> None:
        self.tear_wal()
        code, out, _ = self.run_cli("dump", self.snap, self.wal)
        self.assertEqual(code, DAMAGED)
        self.assertIn("stop: TORN after 2 entries", out)
        self.assertEqual(Path(self.wal).read_bytes(), self.wal_bytes[:-3])

    def test_stats_summarizes_recovery_and_the_wal(self) -> None:
        code, rows = self.json_lines("stats", self.snap, self.wal)
        self.assertEqual(code, CLEAN)
        self.assertEqual(rows[0]["recover"]["count"], 5)
        self.assertEqual(rows[0]["wal"]["entries"], 3)
        self.tear_wal()
        code, rows = self.json_lines("stats", self.snap, self.wal)
        self.assertEqual(code, DAMAGED)
        self.assertEqual(rows[0]["recover"]["count"], 4)
        self.assertEqual(rows[0]["wal"]["stop"], "TORN")

    def test_quarantine_lists_and_shows_records(self) -> None:
        path = self.dir / "q"
        head = b"DKPQ\x01\x00\x00\x00" + struct.pack("<qQQ", 40, 11, 3)
        path.write_bytes(head + b"\x0a\x0b\x0c")
        code, rows = self.json_lines("quarantine", str(path))
        self.assertEqual(code, CLEAN)
        self.assertEqual(rows[0]["wal_offset"], 40)
        self.assertEqual(rows[-1], {"end": "END", "records": 1, "file_bytes": 35})
        code, rows = self.json_lines("quarantine", str(path), "--show", "0")
        self.assertEqual(rows[0]["wal_bytes"], "0a0b0c")
        code, _, err = self.run_cli("quarantine", str(path), "--show", "1")
        self.assertEqual(code, UNOPENED)
        self.assertIn("no record 1", err)
        path.write_bytes(head + b"\x0a")
        code, rows = self.json_lines("quarantine", str(path))
        self.assertEqual((code, rows[-1]["end"]), (DAMAGED, "DAMAGED"))

    def test_a_keyed_file_reads_its_header_but_does_not_check_without_its_key(
        self,
    ) -> None:
        snap, wal = str(self.dir / "k.snap"), str(self.dir / "k.wal")
        _datakit._write_intset(snap, wal, [10], [1], bytes(range(32)))
        code, rows = self.json_lines("inspect", snap)
        self.assertEqual((code, rows[0]["count"]), (CLEAN, 1))
        code, rows = self.json_lines("verify", snap, wal)
        self.assertEqual(code, DAMAGED)
        self.assertEqual((rows[0]["snapshot_ok"], rows[0]["wal_ok"]), (False, False))
        self.assertEqual(rows[0]["error"], "NO_KEY_ATTACHED")

    def test_files_that_do_not_open_exit_2(self) -> None:
        junk = self.dir / "junk"
        junk.write_bytes(b"not persisted")
        for argv in (
            ("inspect", str(junk)),
            ("verify", self.snap, str(self.dir / "s2.snap")),
            ("verify", self.snap, self.snap),
            ("dump", self.snap),
        ):
            with self.subTest(argv=argv):
                code, _, err = self.run_cli(*argv)
                self.assertEqual(code, UNOPENED)
                self.assertIn("cannot open", err)

    def test_runs_as_a_module(self) -> None:
        proc = subprocess.run(
            [sys.executable, "-m", "datakit", "inspect", self.snap],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, CLEAN, proc.stderr)
        self.assertIn("kind: snapshot", proc.stdout)


if __name__ == "__main__":
    unittest.main()

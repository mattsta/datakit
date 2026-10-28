"""Repair: the plan says what apply does, and only copies are ever written."""

from __future__ import annotations

import contextlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from datakit import _datakit, persist
from datakit.__main__ import CLEAN, DAMAGED, UNOPENED, main

KEY = bytes(range(32))


class Repair(unittest.TestCase):
    """An intset snapshot holding {10, 20} and a WAL inserting 1, 2, 3."""

    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)
        self.src = self.dir / "src"
        self.src.mkdir()
        self.snap = self.src / "s.snap"
        self.wal = self.src / "s.wal"
        _datakit._write_intset(self.snap, self.wal, [10, 20], [1, 2, 3])
        self.clean = self.wal.read_bytes()
        self.offsets = [e.offset for e in persist.wal_walk(self.wal).entries]

    def tearDown(self) -> None:
        self._dir.cleanup()

    def flipped_second_entry(self) -> bytes:
        at = self.offsets[1]
        (length,) = struct.unpack_from("<I", self.clean, at)
        bad = bytearray(self.clean)
        bad[at + 4 + length // 2] ^= 0x10
        return bytes(bad)

    def originals(self) -> dict[str, bytes]:
        return {p.name: p.read_bytes() for p in sorted(self.src.iterdir())}

    def test_the_plan_is_what_apply_does_and_the_originals_stay(self) -> None:
        cases = (
            # (WAL bytes, action, cut point, entries kept)
            ("clean", self.clean, "NONE", len(self.clean), 3),
            ("torn", self.clean[:-3], "CUT", self.offsets[2], 2),
            ("corrupt", self.flipped_second_entry(), "HEAL", self.offsets[1], 1),
        )
        for name, data, action, cut, kept in cases:
            with self.subTest(case=name):
                self.wal.write_bytes(data)
                before = self.originals()
                plan = persist.repair_plan(self.snap, self.wal)
                self.assertEqual(
                    (plan.action, plan.cut_at, plan.entries_kept),
                    (action, cut, kept),
                )
                self.assertEqual(plan.bytes_past, len(data) - cut)
                self.assertEqual(plan.rebuilt.count, 2 + kept)

                out = self.dir / f"out-{name}"
                result = persist.repair(self.snap, self.wal, out)
                self.assertEqual(self.originals(), before)
                self.assertTrue(result.synced)
                self.assertEqual(result.count, plan.rebuilt.count)
                self.assertEqual(result.wal.read_bytes(), data[:cut])
                after = persist.wal_walk(result.wal, result.snapshot)
                self.assertEqual((after.stop, len(after.entries)), ("END", kept))
                if action == "HEAL":
                    self.assertEqual(
                        (result.bad_kind, result.bad_action, result.bad_offset),
                        ("CORRUPT", "HEALED", cut),
                    )
                    q = persist.quarantine(result.quarantine)
                    self.assertEqual(q.end, "END")
                    self.assertEqual(
                        [(r.wal_offset, r.wal_bytes) for r in q.records],
                        [(cut, data[cut:])],
                    )
                else:
                    self.assertEqual(result.bad_kind, "NONE")
                    self.assertIsNone(result.quarantine)

    def test_a_held_wal_plans_hold_and_apply_changes_nothing(self) -> None:
        snap, wal = self.src / "k.snap", self.src / "k.wal"
        _datakit._write_intset(snap, wal, [10], [1, 2], KEY)
        plan = persist.repair_plan(snap, wal)
        self.assertEqual((plan.action, plan.bad_kind), ("HOLD", "KEY"))
        result = persist.repair(snap, wal, self.dir / "out")
        self.assertEqual(result.wal.read_bytes(), wal.read_bytes())
        self.assertIsNone(result.quarantine)

    def test_a_quarantine_beside_the_wal_is_carried_and_appended_to(self) -> None:
        old = b"DKPQ\x01\x00\x00\x00" + struct.pack("<qQQ", 99, 7, 2) + b"zz"
        (self.src / "s.quarantine").write_bytes(old)
        self.wal.write_bytes(self.flipped_second_entry())
        result = persist.repair(self.snap, self.wal, self.dir / "out")
        q = persist.quarantine(result.quarantine)
        self.assertEqual([r.wal_offset for r in q.records], [99, self.offsets[1]])
        self.assertEqual((self.src / "s.quarantine").read_bytes(), old)

    def test_repairs_refuse_the_originals_directory_and_existing_files(self) -> None:
        with self.assertRaises(ValueError):
            persist.repair(self.snap, self.wal, self.src)
        out = self.dir / "out"
        out.mkdir()
        (out / "s.wal").write_bytes(b"in the way")
        with self.assertRaises(FileExistsError):
            persist.repair(self.snap, self.wal, out)
        self.assertEqual((out / "s.wal").read_bytes(), b"in the way")
        with self.assertRaises(persist.OpenError):
            persist.repair(self.snap, self.dir / "missing.wal", self.dir / "o2")
        self.assertFalse((self.dir / "o2").exists())

    def run_cli(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_the_commands(self) -> None:
        code, out, _ = self.run_cli("repair", "plan", str(self.wal), str(self.snap))
        self.assertEqual(code, CLEAN, out)
        self.assertIn("nothing to do", out)
        self.wal.write_bytes(self.clean[:-3])
        code, out, _ = self.run_cli(
            "--json", "repair", "plan", str(self.snap), str(self.wal)
        )
        self.assertEqual((code, json.loads(out)["action"]), (DAMAGED, "CUT"))
        out_dir = self.dir / "fixed"
        code, out, _ = self.run_cli(
            "--json",
            "repair",
            "apply",
            str(self.snap),
            str(self.wal),
            "--out",
            str(out_dir),
        )
        row = json.loads(out)
        self.assertEqual(code, CLEAN, out)
        self.assertEqual(row["after"], {"stop": "END", "entries": 2})
        self.assertEqual(row["result"]["wal"], str(out_dir / "s.wal"))
        code, _, err = self.run_cli(
            "repair", "apply", str(self.snap), str(self.wal), "--out", str(out_dir)
        )
        self.assertEqual(code, UNOPENED)
        self.assertIn("refused", err)


if __name__ == "__main__":
    unittest.main()

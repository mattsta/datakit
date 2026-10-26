"""objdiff: parsing llvm-objdump output, address normalization, reports.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from dktools import objdiff
from dktools.gitutil import ToolError

BASE = """\

datakit-test:\tfile format mach-o arm64

Disassembly of section __TEXT,__text:

0000000100012340 <_rapidhash>:
100012340: \tstp\tx29, x30, [sp, #-0x10]!
100012344: \tadrp\tx8, 0x100200000 <_secret>
100012348: \tcmp\tx1, #0x30
10001234c: \tb.hi\t0x100012360 <_rapidhash+0x20>
100012350: \tbl\t0x100099000 <_rapidMixSlow>
100012354: \tret

0000000100012400 <_other>:
100012400: \tret
"""

# The same code placed 16 bytes later, its helper elsewhere.
MOVED = BASE.replace("100012340 <_rapidhash>", "100012350 <_rapidhash>").replace(
    "0x100099000", "0x1000aa000"
)

CHANGED = BASE.replace("cmp\tx1, #0x30", "cmp\tx1, #0x31")


class Parse(unittest.TestCase):
    def test_listing_with_and_without_underscore(self) -> None:
        for name in ("rapidhash", "_rapidhash"):
            listing = objdiff.parse(BASE, name.lstrip("_"))
            assert listing is not None
            self.assertEqual(listing.start, 0x100012340)
            self.assertEqual(len(listing.lines), 6)

    def test_stops_at_the_next_function(self) -> None:
        listing = objdiff.parse(BASE, "rapidhash")
        assert listing is not None
        self.assertEqual(listing.lines[-1], "ret")
        other = objdiff.parse(BASE, "other")
        assert other is not None
        self.assertEqual(other.lines, ("ret",))

    def test_absent_symbol(self) -> None:
        self.assertIsNone(objdiff.parse(BASE, "missing"))

    def test_placement_is_offset_in_the_fetch_line(self) -> None:
        listing = objdiff.parse(MOVED, "rapidhash")
        assert listing is not None
        self.assertEqual(listing.placement, "@0x100012350+16")


class Normalize(unittest.TestCase):
    def test_branch_keeps_only_the_symbolic_target(self) -> None:
        self.assertEqual(
            objdiff.normalize("b.hi\t0x100012360 <_rapidhash+0x20>"),
            "b.hi <_rapidhash+0x20>",
        )

    def test_page_addresses_drop(self) -> None:
        self.assertEqual(objdiff.normalize("adrp\tx8, 0x100200000"), "adrp x8, ADDR")

    def test_small_immediates_stay(self) -> None:
        self.assertEqual(objdiff.normalize("cmp\tx1, #0x30"), "cmp x1, #0x30")


class Compare(unittest.TestCase):
    def test_moved_code_is_identical(self) -> None:
        base = objdiff.parse(BASE, "rapidhash")
        moved = objdiff.parse(MOVED, "rapidhash")
        assert base is not None and moved is not None
        report = objdiff.compare(base, moved)
        self.assertEqual(
            report,
            [
                "rapidhash: base @0x100012340+0 6 insns, "
                "current @0x100012350+16 6 insns, identical"
            ],
        )

    def test_changed_code_differs_with_a_diff(self) -> None:
        base = objdiff.parse(BASE, "rapidhash")
        changed = objdiff.parse(CHANGED, "rapidhash")
        assert base is not None and changed is not None
        report = objdiff.compare(base, changed)
        self.assertTrue(report[0].endswith("differs"))
        self.assertIn("  -cmp x1, #0x30", report)
        self.assertIn("  +cmp x1, #0x31", report)


class Binary(unittest.TestCase):
    def test_finds_the_one_side_binary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            program = root / "perfcompare/run/base/build-clang22-perf/src/datakit-test"
            program.parent.mkdir(parents=True)
            program.write_bytes(b"")
            self.assertEqual(objdiff.binary(root, "run", "base"), program)
            with self.assertRaises(ToolError):
                objdiff.binary(root, "run", "current")


if __name__ == "__main__":
    unittest.main()

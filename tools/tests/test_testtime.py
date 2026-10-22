"""``dktools testtime``: section timing, the report, and exit codes.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

from dktools import procs, testtime


class Sections(unittest.TestCase):
    def test_sections_run_header_to_header_and_to_the_end(self) -> None:
        lines = [
            (0.0, "preamble\n"),
            (1.0, "test — first\n"),
            (1.5, "noise\n"),
            (4.0, "test — second\n"),
            (4.5, "test — third\n"),
        ]
        got = testtime.sections(lines, end=10.0)
        self.assertEqual(
            got,
            [
                testtime.Section("first", 3.0),
                testtime.Section("second", 0.5),
                testtime.Section("third", 5.5),
            ],
        )
        self.assertEqual(testtime.sections([(0.0, "no headers\n")], 1.0), [])

    def test_extra_prefixes_open_sections_too(self) -> None:
        lines = [(0.0, "Test own header:\n"), (2.0, "test — harness\n")]
        got = testtime.sections(lines, 3.0, (testtime.SECTION_PREFIX, "Test "))
        self.assertEqual(
            got,
            [testtime.Section("own header:", 2.0), testtime.Section("harness", 1.0)],
        )

    def test_render_lists_the_slowest_first(self) -> None:
        found = [
            testtime.Section("a", 1.0),
            testtime.Section("b", 6.0),
            testtime.Section("c", 3.0),
        ]
        text = testtime.render(found, 10.0, top=2)
        lines = text.splitlines()
        self.assertIn("10.00s total, 3 sections", lines[0])
        single = lines[lines.index("slowest sections:") + 1 :]
        self.assertTrue(single[0].endswith("b") and "60.0%" in single[0])
        self.assertTrue(single[1].endswith("c"))
        self.assertEqual(len(single), 2)

    def test_groups_mask_numbers(self) -> None:
        found = [
            testtime.Section("8 bytes max with 64 pairs", 1.0),
            testtime.Section("16 bytes max with 64 pairs", 2.0),
            testtime.Section("other", 0.5),
        ]
        self.assertEqual(
            testtime.groups(found),
            [("# bytes max with # pairs", 2, 3.0), ("other", 1, 0.5)],
        )


class Run(unittest.TestCase):
    """End to end against a fake test binary."""

    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)

    def tearDown(self) -> None:
        self._dir.cleanup()

    def fake(self, body: str) -> Path:
        script = self.dir / "fake-test"
        script.write_text(f"#!{sys.executable}\nimport sys, time\n{body}\n")
        os.chmod(script, 0o755)
        return script

    def run_tool(self, binary: Path, timeout: float = 30.0) -> tuple[int, str]:
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = testtime.run(binary, "x", timeout, top=5)
        return code, out.getvalue() + err.getvalue()

    def test_a_passing_test_reports_its_sections(self) -> None:
        binary = self.fake(
            "assert sys.argv[1:] == ['test', 'x']\n"
            "print('test — quick', flush=True)\n"
            "print('test — slow', flush=True)\n"
            "time.sleep(0.3)\n"
            "print('ALL TESTS PASSED!')"
        )
        code, out = self.run_tool(binary)
        self.assertEqual(code, 0, out)
        report = out.splitlines()
        self.assertIn("2 sections", report[0])
        single = report[report.index("slowest sections:") + 1 :]
        self.assertTrue(single[0].endswith("slow"), out)

    def test_a_failing_test_exits_1(self) -> None:
        binary = self.fake("print('test — a', flush=True)\nsys.exit(3)")
        code, out = self.run_tool(binary)
        self.assertEqual(code, 1)
        self.assertIn("failed (exit 3)", out)

    def test_a_hung_test_is_stopped(self) -> None:
        binary = self.fake("print('test — hang', flush=True)\ntime.sleep(60)")
        start = time.monotonic()
        code, out = self.run_tool(binary, timeout=0.5)
        self.assertEqual(code, 1)
        self.assertIn("ran past", out)
        self.assertLess(time.monotonic() - start, 20.0)

    def test_a_missing_binary_is_an_input_error(self) -> None:
        with self.assertRaises(procs.ToolError):
            testtime.run(self.dir / "absent", "x", 1.0, 5)


if __name__ == "__main__":
    unittest.main()

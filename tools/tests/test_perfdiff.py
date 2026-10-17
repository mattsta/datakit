"""Exit-code contract of ``dktools perfdiff``.

0 clean, 1 regression, 2 any input error, and never a traceback.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import os
import tempfile
import unittest
from pathlib import Path

from dktools import perfdiff


def perf_row(module: str, name: str, nbytes: int, ns: str, mb: str) -> str:
    return f"PERF\t{module}\t{name}\t{nbytes}\t1000\t{ns}\t{mb}\n"


class PerfDiffExitCodes(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)

    def tearDown(self) -> None:
        self._dir.cleanup()

    def write(self, name: str, text: str | bytes) -> Path:
        path = self.dir / name
        if isinstance(text, bytes):
            path.write_bytes(text)
        else:
            path.write_text(text, encoding="utf-8")
        return path

    def run_perfdiff(
        self, baseline: Path, current: Path, tolerance: float = 10.0
    ) -> tuple[int, str, str]:
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = perfdiff.run(baseline, current, tolerance)
        return code, out.getvalue(), err.getvalue()

    def test_clean_and_regression(self) -> None:
        base = self.write("base.tsv", perf_row("m", "a", 64, "100.0", "640.0"))
        same = self.write("same.tsv", perf_row("m", "a", 64, "105.0", "610.0"))
        slow = self.write("slow.tsv", perf_row("m", "a", 64, "150.0", "640.0"))
        self.assertEqual(self.run_perfdiff(base, same)[0], 0)
        code, out, _ = self.run_perfdiff(base, slow)
        self.assertEqual(code, 1)
        self.assertIn("REGRESSION\tm\ta\t64", out)

    def test_zero_baseline_metric_is_not_compared(self) -> None:
        # A zero-byte row prints 0.0 MB/s, and a sub-0.05 ns op prints
        # 0.0 ns: neither may divide by zero.
        base = self.write(
            "base.tsv",
            perf_row("m", "empty", 0, "12.0", "0.0")
            + perf_row("m", "tiny", 8, "0.0", "900.0")
            + perf_row("m", "both", 0, "0.0", "0.0"),
        )
        cur = self.write(
            "cur.tsv",
            perf_row("m", "empty", 0, "12.5", "0.0")
            + perf_row("m", "tiny", 8, "0.1", "890.0")
            + perf_row("m", "both", 0, "3.0", "0.0"),
        )
        code, _, err = self.run_perfdiff(base, cur)
        self.assertEqual(code, 0)
        self.assertIn("# skipped\tm\tboth\t0", err)

        # The remaining metric still catches a regression.
        slow = self.write(
            "slow.tsv",
            perf_row("m", "empty", 0, "20.0", "0.0")
            + perf_row("m", "tiny", 8, "0.0", "900.0")
            + perf_row("m", "both", 0, "0.0", "0.0"),
        )
        code, out, _ = self.run_perfdiff(base, slow)
        self.assertEqual(code, 1)
        self.assertIn("REGRESSION\tm\tempty\t0", out)

    def test_input_errors_exit_2(self) -> None:
        good = self.write("good.tsv", perf_row("m", "a", 64, "100.0", "640.0"))
        cases = {
            "missing file": self.dir / "absent.tsv",
            "directory": self.dir,
            "not utf-8": self.write("bin.tsv", b"PERF\tm\t\xff\xfe\n"),
            "malformed row": self.write("short.tsv", "PERF\tm\ta\t64\n"),
            "unparsable number": self.write(
                "word.tsv", perf_row("m", "a", 64, "fast", "640.0")
            ),
            "nan metric": self.write("nan.tsv", perf_row("m", "a", 64, "nan", "640.0")),
            "infinite metric": self.write(
                "inf.tsv", perf_row("m", "a", 64, "100.0", "inf")
            ),
            "negative metric": self.write(
                "neg.tsv", perf_row("m", "a", 64, "-1.0", "640.0")
            ),
        }
        unreadable = self.write("locked.tsv", perf_row("m", "a", 64, "1", "1"))
        unreadable.chmod(0)
        if not os.access(unreadable, os.R_OK):
            cases["permission denied"] = unreadable

        for label, bad in cases.items():
            for baseline, current in ((bad, good), (good, bad)):
                with self.subTest(label=label, baseline=baseline.name):
                    code, out, err = self.run_perfdiff(baseline, current)
                    self.assertEqual(code, 2)
                    self.assertEqual(out, "")
                    self.assertNotIn("Traceback", err)
                    self.assertTrue(err.strip())

        for tolerance in (float("nan"), float("inf"), -1.0):
            with self.subTest(tolerance=tolerance):
                self.assertEqual(self.run_perfdiff(good, good, tolerance)[0], 2)


if __name__ == "__main__":
    unittest.main()

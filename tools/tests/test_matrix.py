"""``dktools matrix``: MATRIX rows parsed, medians over runs, rates
formatted, the per-kind-and-size tables.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from dktools import matrix
from dktools.gitutil import ToolError

HEADER = "MATRIX\tcontainer\tkind\tsize\top\tops\tseconds\tbytes_per_element"


def row(
    container: str, kind: str, size: int, op: str, secs: float, nbytes: float
) -> str:
    return (
        f"MATRIX\t{container}\t{kind}\t{size}\t{op}\t{size}\t{secs:.9f}\t{nbytes:.2f}"
    )


def output(*rows: str) -> str:
    return "\n".join(
        [
            "noise before",
            HEADER,
            "MATRIXSHAPE\tmm\tkey -> value",
            *rows,
            "ALL TESTS PASSED!",
        ]
    )


class Parse(unittest.TestCase):
    def test_rows_and_shapes(self) -> None:
        run = matrix.parse(output(row("mm", "str16", 100, "read", 0.001, 20.5)))
        self.assertEqual(run.rows, {("mm", "str16", 100, "read"): (100, 0.001, 20.5)})
        self.assertEqual(run.shapes, {"mm": "key -> value"})

    def test_malformed_row_names_its_line(self) -> None:
        with self.assertRaisesRegex(ToolError, r"out:4: malformed MATRIX row"):
            matrix.parse(output("MATRIX\tmm\tstr16\tmany\tread\t1\t1\t1"), "out")

    def test_unknown_op_is_malformed(self) -> None:
        with self.assertRaises(ToolError):
            matrix.parse(output(row("mm", "str16", 100, "upsert", 0.001, 1)))


class Medians(unittest.TestCase):
    def test_median_seconds_give_the_rate(self) -> None:
        runs = [
            matrix.parse(output(row("mm", "k", 1000, "read", s, b)))
            for s, b in ((0.001, 10), (0.004, 12), (0.002, 11))
        ]
        cell = matrix.medians(runs)[("mm", "k", 1000, "read")]
        self.assertAlmostEqual(cell.ops_per_sec, 1000 / 0.002)
        self.assertEqual(cell.bytes_per_element, 11)

    def test_keys_missing_from_a_run_are_left_out(self) -> None:
        a = matrix.parse(
            output(row("mm", "k", 10, "read", 1, 1), row("mm", "k", 10, "create", 1, 1))
        )
        b = matrix.parse(output(row("mm", "k", 10, "read", 1, 1)))
        self.assertEqual(list(matrix.medians([a, b])), [("mm", "k", 10, "read")])


class Rate(unittest.TestCase):
    def test_three_significant_digits(self) -> None:
        self.assertEqual(matrix.rate(12_345_678), "12.3M")
        self.assertEqual(matrix.rate(850_000), "850K")
        self.assertEqual(matrix.rate(4_100_000_000), "4.10G")
        self.assertEqual(matrix.rate(512), "512")
        self.assertEqual(matrix.rate(1.5), "1.50")


class Tables(unittest.TestCase):
    def test_best_in_bold_and_dash_without_update(self) -> None:
        rows = []
        for op, secs in (
            ("create", 0.01),
            ("read", 0.001),
            ("update", 0.002),
            ("delete", 0.01),
        ):
            rows.append(row("fast", "int", 1000, op, secs, 40))
        for op, secs in (("create", 0.02), ("read", 0.002), ("delete", 0.005)):
            rows.append(row("small", "int", 1000, op, secs, 8))
        cells = matrix.medians([matrix.parse(output(*rows))])
        lines = matrix.tables(cells, {"fast": "f", "small": "s"})
        self.assertIn("### int, 1,000 elements", lines)
        fast = next(line for line in lines if line.startswith("| fast "))
        small = next(line for line in lines if line.startswith("| small "))
        self.assertEqual(
            fast, "| fast | f | **100K** | **1.00M** | **500K** | 100K | 40.0 |"
        )
        self.assertEqual(small, "| small | s | 50.0K | 500K | — | **200K** | **8.0** |")


class LoadRuns(unittest.TestCase):
    def test_runs_in_order_and_empty_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            with self.assertRaisesRegex(ToolError, "no run-N.txt"):
                matrix.load_runs(work)
            for n, secs in ((2, 0.2), (10, 1.0), (1, 0.1)):
                (work / f"run-{n}.txt").write_text(
                    output(row("mm", "k", 10, "read", secs, 1))
                )
            runs = matrix.load_runs(work)
            self.assertEqual(
                [r.rows[("mm", "k", 10, "read")][1] for r in runs], [0.1, 0.2, 1.0]
            )


if __name__ == "__main__":
    unittest.main()

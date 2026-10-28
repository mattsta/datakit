"""The comparison logic of ``dktools perfcompare``.

Building and running the two trees is exercised by using the command;
these tests pin what it computes from the runs: module parsing, the
per-row medians, the change, the table and the tolerance verdict, the
preset each compiler gets, and the input refusals before any build.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest
from pathlib import Path

from dktools import perfcompare, toolchain
from dktools.gitutil import ToolError
from dktools.perfdiff import PerfRow


def row(module: str, name: str, ns: float, nbytes: int = 0) -> PerfRow:
    return PerfRow(module, name, nbytes, 1000, ns, 0.0)


def run_of(*rows: PerfRow) -> dict[tuple[str, str, int], PerfRow]:
    return {r.key: r for r in rows}


class ParseModules(unittest.TestCase):
    def test_none_means_every_module(self) -> None:
        self.assertIsNone(perfcompare.parse_modules(None))

    def test_list_passes_through(self) -> None:
        self.assertEqual(
            perfcompare.parse_modules("multidict,multiOrderedSet"),
            "multidict,multiOrderedSet",
        )

    def test_refuses_empty_or_odd_names(self) -> None:
        for bad in ("", "multidict,", "a b", "x;y", ",multidict"):
            with self.assertRaises(ToolError, msg=bad):
                perfcompare.parse_modules(bad)


class Medians(unittest.TestCase):
    def test_median_per_key_over_runs_that_have_it(self) -> None:
        runs = [
            run_of(row("m", "a", 100.0), row("m", "b", 10.0)),
            run_of(row("m", "a", 300.0)),
            run_of(row("m", "a", 200.0), row("m", "b", 30.0)),
        ]
        got = perfcompare.medians(runs)
        self.assertEqual(got[("m", "a", 0)], 200.0)
        self.assertEqual(got[("m", "b", 0)], 20.0)


class Table(unittest.TestCase):
    def test_change_and_zero_base(self) -> None:
        self.assertAlmostEqual(perfcompare.change_pct(200.0, 150.0), -25.0)
        self.assertIsNone(perfcompare.change_pct(0.0, 5.0))

    def test_rows_one_sided_rows_and_tolerance(self) -> None:
        base = {("m", "a", 0): 100.0, ("m", "b", 0): 100.0, ("m", "gone", 0): 1.0}
        current = {("m", "a", 0): 80.0, ("m", "b", 0): 125.0, ("m", "new", 0): 2.0}
        lines, regressed = perfcompare.table(base, current, 10.0)
        text = "\n".join(lines)
        self.assertIn("-20.0%", text)
        self.assertIn("+25.0%", text)
        self.assertIn("base only: m gone 0", text)
        self.assertIn("current only: m new 0", text)
        self.assertEqual(regressed, [("m", "b", 0)])

    def test_spread_shows_each_sides_lowest_and_highest_run(self) -> None:
        def side(*ns: float) -> list:
            return [run_of(row("m", "a", v)) for v in ns]

        base_runs = side(100.0, 90.0, 140.0)
        current_runs = side(80.0, 85.0, 75.0)
        self.assertEqual(perfcompare.ranges(base_runs), {("m", "a", 0): (90.0, 140.0)})
        lines, _ = perfcompare.table(
            perfcompare.medians(base_runs),
            perfcompare.medians(current_runs),
            None,
            (perfcompare.ranges(base_runs), perfcompare.ranges(current_runs)),
        )
        self.assertIn("base range", lines[0])
        self.assertIn("[90.0-140.0]", lines[1])
        self.assertIn("[75.0-85.0]", lines[1])
        # Without it, the table is as before
        plain, _ = perfcompare.table(
            perfcompare.medians(base_runs), perfcompare.medians(current_runs), None
        )
        self.assertNotIn("range", plain[0])

    def test_no_tolerance_never_regresses(self) -> None:
        _, regressed = perfcompare.table(
            {("m", "a", 0): 1.0}, {("m", "a", 0): 50.0}, None
        )
        self.assertEqual(regressed, [])


class Presets(unittest.TestCase):
    def test_perf_where_built_else_default(self) -> None:
        clang = toolchain.resolve_compiler("clang22")
        gcc = toolchain.resolve_compiler("gcc15")
        self.assertEqual(perfcompare.perf_preset(clang), "perf")
        self.assertIsNone(perfcompare.perf_preset(gcc))


class Refusals(unittest.TestCase):
    def options(self, **kw: object) -> perfcompare.PerfCompareOptions:
        opts = perfcompare.PerfCompareOptions(name="t", base="HEAD")
        for key, value in kw.items():
            setattr(opts, key, value)
        return opts

    def test_bad_inputs_refuse_before_building(self) -> None:
        repo = Path(__file__).resolve().parents[2]
        for kw in (
            {"runs": 0},
            {"jobs": 0},
            {"tolerance": -1.0},
            {"modules": "bad name"},
            {"name": "../escape"},
            {"bench_paths": ["/abs/path.c"]},
            {"current_defines": ("NoValue",)},
            {"current": "no-such-revision-anywhere"},
        ):
            with self.assertRaises(ToolError, msg=str(kw)):
                perfcompare.run(repo, self.options(**kw))


class CurrentLabel(unittest.TestCase):
    def test_working_tree_or_revision_with_bench_paths(self) -> None:
        bench: list[tuple[str, None]] = [("src/perfRegression.c", None)]
        opts = perfcompare.PerfCompareOptions(name="t", base="HEAD~1")
        self.assertEqual(
            perfcompare.current_label(opts, None, "abcdef0123456789", bench),
            "HEAD + working tree (abcdef0123)",
        )
        opts.current = "HEAD"
        self.assertEqual(
            perfcompare.current_label(opts, "0123456789abcdef", "ffff", bench),
            "HEAD (0123456789) + src/perfRegression.c",
        )


if __name__ == "__main__":
    unittest.main()

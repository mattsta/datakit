"""The comparison logic of ``dktools perfcompare``.

Building and running the two trees is exercised by using the command;
these tests pin what it computes from the runs: module parsing, the
per-row medians, the change, the table and the tolerance verdict, the
preset each compiler gets, and the input refusals before any build.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import tempfile
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

    def test_a_row_whose_bytes_changed_is_still_compared(self) -> None:
        base = {("m", "heap", 2800): 130.0, ("m", "gone", 5): 1.0}
        current = {("m", "heap", 2400): 117.0, ("m", "new", 6): 2.0}
        lines, regressed = perfcompare.table(base, current, 5.0)
        text = "\n".join(lines)
        self.assertIn("-10.0%", text)
        self.assertIn("bytes -> 2400 (-14.3%)", text)
        self.assertNotIn("base only: m heap", text)
        self.assertNotIn("current only: m heap", text)
        # Different names never pair
        self.assertIn("base only: m gone 5", text)
        self.assertIn("current only: m new 6", text)
        self.assertEqual(regressed, [])

    def test_bytes_that_vary_between_runs_settle_on_the_median(self) -> None:
        runs = [
            run_of(row("m", "heap", 10.0, 1000), row("m", "fixed", 1.0, 5)),
            run_of(row("m", "heap", 12.0, 1010), row("m", "fixed", 1.0, 5)),
            run_of(row("m", "heap", 11.0, 990), row("m", "fixed", 1.0, 5)),
        ]
        settled = perfcompare.settle_bytes(runs)
        self.assertEqual(
            perfcompare.medians(settled),
            {("m", "heap", 1000): 11.0, ("m", "fixed", 5): 1.0},
        )

    def test_a_name_a_run_reports_twice_keeps_its_keys(self) -> None:
        runs = [
            run_of(row("m", "x", 1.0, 1), row("m", "x", 2.0, 2)),
            run_of(row("m", "x", 1.0, 3), row("m", "x", 2.0, 4)),
        ]
        self.assertEqual(perfcompare.settle_bytes(runs), runs)

    def test_a_name_on_one_side_twice_is_not_paired(self) -> None:
        base = {("m", "x", 1): 1.0, ("m", "x", 2): 1.0}
        current = {("m", "x", 3): 1.0}
        self.assertEqual(perfcompare.bytes_changed(base, current), {})

    def test_rank_sum_separates_shifted_runs_and_not_one_stray_run(self) -> None:
        base = [100.0 + i for i in range(11)]
        shifted = [120.0 + i for i in range(11)]
        p = perfcompare.rank_sum_p(base, shifted)
        assert p is not None
        self.assertLess(p, 0.001)
        self.assertEqual(perfcompare.verdict(base, shifted), "slower")
        self.assertEqual(perfcompare.verdict(shifted, base), "faster")
        # The same runs, one of them stray: no verdict either way
        stray = [*base[:-1], 300.0]
        self.assertEqual(perfcompare.verdict(base, stray), "~")
        # Identical runs, ties throughout
        same = [5.0] * 11
        self.assertEqual(perfcompare.rank_sum_p(same, same), 1.0)
        # Too few runs to tell
        self.assertIsNone(perfcompare.rank_sum_p(base[:4], shifted[:4]))
        self.assertEqual(perfcompare.verdict(base[:4], shifted[:4]), "-")

    def test_five_runs_a_side_that_never_overlap_give_a_verdict(self) -> None:
        # The default run count: fully separated runs have the exact
        # two-sided p 2 / C(10, 5) = 2/252, below the 0.01 threshold
        base = [100.0, 101.0, 102.0, 103.0, 104.0]
        faster = [50.0, 51.0, 52.0, 53.0, 54.0]
        self.assertAlmostEqual(perfcompare.rank_sum_p(base, faster), 2 / 252)
        self.assertEqual(perfcompare.verdict(base, faster), "faster")
        self.assertEqual(perfcompare.verdict(faster, base), "slower")
        # One run of the five crossing over leaves it unproven at 0.01
        crossed = [50.0, 51.0, 52.0, 53.0, 100.5]
        p = perfcompare.rank_sum_p(base, crossed)
        assert p is not None
        self.assertAlmostEqual(p, 4 / 252)
        self.assertEqual(perfcompare.verdict(base, crossed), "~")

    def test_exact_and_normal_p_agree_where_both_apply(self) -> None:
        # Past the exact limit the normal approximation takes over; at
        # moderate sizes the two are close
        a = [float(i) for i in range(0, 40, 2)]
        b = [float(i) for i in range(1, 41, 2)]
        exact = perfcompare.rank_sum_p(a, b)
        normal = perfcompare.rank_sum_normal_p(a, b)
        assert exact is not None
        self.assertAlmostEqual(exact, normal, delta=0.02)

    def test_a_regression_past_tolerance_needs_a_slower_verdict(self) -> None:
        key = ("m", "a", 0)
        base = [100.0 + i for i in range(11)]
        # The median moves past the tolerance (105 -> 107), the ranks barely
        noisy = [100.0, 101.0, 102.0, 103.0, 104.0, 107.0, 108.0, 109.0, 110.0]
        noisy += [111.0, 400.0]
        slower = [130.0 + i for i in range(11)]
        for runs, want in ((noisy, []), (slower, [key])):
            with self.subTest(want=want):
                lines, regressed = perfcompare.table(
                    {key: perfcompare.statistics.median(base)},
                    {key: perfcompare.statistics.median(runs)},
                    0.5,
                    None,
                    ({key: base}, {key: runs}),
                )
                self.assertIn("verdict", lines[0])
                self.assertGreater(
                    perfcompare.change_pct(
                        perfcompare.statistics.median(base),
                        perfcompare.statistics.median(runs),
                    ),
                    0.5,
                )
                self.assertEqual(regressed, want)

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
        self.assertEqual(
            perfcompare.current_label(None, None, "abcdef0123456789", bench),
            "HEAD + working tree (abcdef0123)",
        )
        self.assertEqual(
            perfcompare.current_label("HEAD", "0123456789abcdef", "ffff", bench),
            "HEAD (0123456789) + src/perfRegression.c",
        )


if __name__ == "__main__":
    unittest.main()


class Renames(unittest.TestCase):
    def test_a_renamed_module_meets_its_replacement(self) -> None:
        renames = perfcompare.parse_renames(["oldMod=newMod"])
        self.assertEqual(renames, {"oldMod": "newMod"})
        runs = [run_of(row("oldMod", "find", 10.0), row("kept", "find", 5.0))]
        renamed = perfcompare.rename_modules(runs, renames)
        self.assertEqual(set(renamed[0]), {("newMod", "find", 0), ("kept", "find", 0)})
        self.assertEqual(renamed[0][("newMod", "find", 0)].module, "newMod")

    def test_a_malformed_rename_is_refused(self) -> None:
        for text in ("noequals", "=new", "old=", "old=bad name"):
            with self.subTest(text=text), self.assertRaises(ToolError):
                perfcompare.parse_renames([text])


class Reuse(unittest.TestCase):
    def test_runs_load_per_side_in_run_order(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            with self.assertRaisesRegex(ToolError, "--reuse"):
                perfcompare.load_runs(work)
            for side in ("base", "current"):
                for n, ns in ((2, 20.0), (10, 100.0), (1, 10.0)):
                    (work / f"run-{side}-{n}.tsv").write_text(
                        f"PERF\tm\tx\t0\t1000\t{ns}\t0.0\n"
                    )
            runs = perfcompare.load_runs(work)
            self.assertEqual(
                [r[("m", "x", 0)].ns_per_op for r in runs["base"]],
                [10.0, 20.0, 100.0],
            )
            self.assertEqual(len(runs["current"]), 3)

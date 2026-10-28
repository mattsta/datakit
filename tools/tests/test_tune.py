"""``dktools tune``: sweeps out of perfRegression runs, ratios against the
baseline, per-run noise, keep-or-change decisions, the report, cache
sizes.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path

from dktools import perfdiff, tune
from dktools.gitutil import ToolError

REPO = Path(__file__).resolve().parents[2]

#: Five runs that scale every cell alike: ratios between limits are the
#: same in every run, so every-run checks see the medians
EVEN = (0.98, 1.0, 1.02, 0.99, 1.01)


def sweep_of(
    family: str,
    ns: dict[int, dict[str, float]],
    nbytes: dict[int, int] | None = None,
    scale: tuple[float, ...] = EVEN,
) -> tune.Sweep:
    """A sweep whose run r is 'ns' times scale[r]"""
    return tune.Sweep(
        family,
        {
            limit: {op: [v * s for s in scale] for op, v in per.items()}
            for limit, per in ns.items()
        },
        dict(nbytes or {}),
    )


#: multiOrderedSet from a measured run: 4096 ties the default on every mix
#: (getRank dominates the mean) while add is 29% slower there
MOS = {
    256: {"add": 847.6, "getScore": 139.0, "getRank": 24078.1, "remove": 830.8},
    1024: {"add": 934.7, "getScore": 126.9, "getRank": 3996.5, "remove": 757.4},
    2048: {"add": 939.5, "getScore": 141.7, "getRank": 2089.5, "remove": 765.7},
    4096: {"add": 1209.9, "getScore": 126.5, "getRank": 1752.3, "remove": 857.4},
}
MOS_BYTES = {256: 7391175, 1024: 7088801, 2048: 7031699, 4096: 7008639}

#: multimap from a measured run: 1024 is 12-15% faster than the default,
#: short of a clear win
MULTIMAP = {
    512: {"insert": 712.8, "exists": 546.7, "delete": 617.9},
    1024: {"insert": 728.6, "exists": 527.7, "delete": 597.5},
    2048: {"insert": 771.2, "exists": 618.1, "delete": 688.5},
    4096: {"insert": 1004.7, "exists": 774.0, "delete": 905.0},
}
MULTIMAP_BYTES = {512: 2427742, 1024: 2336757, 2048: 2291032, 4096: 2264514}

#: multidict's load sweep in the measured shape: 200 is faster and much
#: larger, 3200 smaller and much slower, against the default 800
MULTIDICT = {
    200: {"add": 290.0, "find": 57.0, "findScattered": 68.0, "delete": 125.0},
    800: {"add": 300.0, "find": 75.0, "findScattered": 89.0, "delete": 140.0},
    3200: {"add": 380.0, "find": 125.0, "findScattered": 143.0, "delete": 180.0},
}
MULTIDICT_BYTES = {200: 9262736, 800: 6300592, 3200: 5084992}


class Sweeps(unittest.TestCase):
    def test_rows_group_by_family_op_and_limit_in_run_order(self) -> None:
        def run(insert256: float, extra: bool) -> dict:
            rows = [
                ("multimapLimits", "insert256", 2000, insert256),
                ("multimapLimits", "exists256", 0, 500.0),
                ("multilistLimits", "popHead2048", 0, 60.0),
                ("multimap", "insert", 0, 1.0),  # not a sweep module
                ("multimapLimits", "oddName", 0, 1.0),  # no limit suffix
            ]
            if extra:  # a row only one run has
                rows.append(("multimapLimits", "delete256", 0, 9.0))
            return {
                (m, n, b): perfdiff.PerfRow(m, n, b, 1, ns, 0.0) for m, n, b, ns in rows
            }

        found = tune.sweeps([run(700.0, True), run(710.0, False)])
        self.assertEqual(sorted(found), ["multilist", "multimap"])
        mm = found["multimap"]
        self.assertEqual(mm.family, "multimap")
        self.assertEqual(
            mm.samples[256], {"insert": [700.0, 710.0], "exists": [500.0, 500.0]}
        )
        self.assertEqual(mm.ns[256], {"insert": 705.0, "exists": 500.0})
        self.assertEqual(mm.bytes, {256: 2000})
        self.assertEqual(mm.runs(), 2)
        self.assertEqual(tune.sweeps([]), {})

    def test_rows_whose_bytes_vary_between_runs_are_kept(self) -> None:
        # A hashed container's filled size differs a little run to run;
        # families and operations keep the order the run printed them in
        def run(add_bytes: int) -> dict:
            rows = [
                ("multimapLimits", "insert2048", 900, 70.0),
                ("multimapLimits", "exists2048", 0, 50.0),
                ("multiOrderedSetLimits", "add2048", add_bytes, 90.0),
                ("multiOrderedSetLimits", "getScore2048", 0, 10.0),
            ]
            return {
                (m, n, b): perfdiff.PerfRow(m, n, b, 1, ns, 0.0) for m, n, b, ns in rows
            }

        found = tune.sweeps([run(1000), run(1004), run(1002)])
        self.assertEqual(list(found), ["multimap", "multiOrderedSet"])
        mos = found["multiOrderedSet"]
        self.assertEqual(mos.ops(), ["add", "getScore"])
        self.assertEqual(mos.samples[2048]["add"], [90.0, 90.0, 90.0])
        self.assertEqual(mos.bytes, {2048: 1002})

    def test_a_family_of_several_containers_sums_them(self) -> None:
        # Operations on one container report its bytes; the limit's bytes
        # are the containers' total, and each container is listed
        rows = [
            ("flexSlackLimits", "dictAdd0", 500, 10.0),
            ("flexSlackLimits", "dictFind0", 500, 5.0),
            ("flexSlackLimits", "listPush0", 300, 8.0),
            ("flexSlackLimits", "dictAdd25", 520, 9.0),
            ("flexSlackLimits", "dictFind25", 520, 5.0),
            ("flexSlackLimits", "listPush25", 360, 4.0),
        ]
        run = {(m, n, b): perfdiff.PerfRow(m, n, b, 1, ns, 0.0) for m, n, b, ns in rows}
        slack = tune.sweeps([run])["flexSlack"]
        self.assertEqual(slack.bytes, {0: 800, 25: 880})
        self.assertEqual(slack.containers(25), {"dictAdd": 520, "listPush": 360})
        lines = tune._container_lines(slack, 25)
        self.assertEqual(
            lines[1:],
            [
                "    0  dictAdd 500 -3.8%, listPush 300 -16.7%",
                "    25* dictAdd 520, listPush 360",
            ],
        )
        # one container: no breakdown
        self.assertEqual(
            tune._container_lines(tune.Sweep("", {}, {}, {0: {"a": 5, "b": 5}}), 0),
            [],
        )

    def test_versus_compares_each_run_with_itself(self) -> None:
        # Run 2 is twice as slow everywhere: each run's ratio is the same
        sweep = tune.Sweep(
            "",
            {
                1024: {"a": [80.0, 160.0, 90.0]},
                2048: {"a": [100.0, 200.0, 100.0]},
            },
        )
        vs = sweep.versus(tune.MIXES[0], 2048)
        self.assertEqual(vs[2048], tune.Spread(1.0, 1.0, 1.0))
        self.assertAlmostEqual(vs[1024].median, 0.8)
        self.assertAlmostEqual(vs[1024].low, 0.8)
        self.assertAlmostEqual(vs[1024].high, 0.9)

    def test_a_mix_is_its_time_weighted_mean(self) -> None:
        sweep = sweep_of(
            "multimap",
            {
                256: {"insert": 300.0, "exists": 100.0, "delete": 300.0},
                2048: {"insert": 100.0, "exists": 300.0, "delete": 100.0},
            },
        )
        read80, write80 = tune.MIXES[1], tune.MIXES[2]
        # read80: exists 0.8, insert and delete 0.1 each: 140 against 260
        self.assertAlmostEqual(sweep.versus(read80, 2048)[256].median, 140 / 260)
        self.assertAlmostEqual(sweep.versus(write80, 2048)[256].median, 260 / 140)

    def test_op_change_and_noise(self) -> None:
        sweep = tune.Sweep(
            "",
            {
                1024: {"a": [90.0, 110.0, 100.0]},
                2048: {"a": [100.0, 100.0, 100.0]},
            },
        )
        self.assertAlmostEqual(sweep.op_change(2048)[1024]["a"], 0.0)
        self.assertEqual(sweep.op_change(2048)[2048], {"a": 0.0})
        # 1024 spreads +-10%, 2048 not at all: the median over limits is 5%
        self.assertAlmostEqual(sweep.noise()["a"], 0.05)
        self.assertEqual(tune.Sweep.of_medians({1: {"a": 1.0}}).noise(), {})


class Decisions(unittest.TestCase):
    def test_the_measured_multiorderedset_keeps_its_default(self) -> None:
        sweep = sweep_of("multiOrderedSet", MOS, MOS_BYTES)
        for mix in tune.MIXES:
            with self.subTest(mix=mix.name):
                d = sweep.decide(mix, 2048)
                self.assertFalse(d.change)
                self.assertEqual((d.why, d.limit), ("keep", 2048))
        # 4096 holds 0.3% fewer bytes, but add is 29% slower there
        self.assertGreater(sweep.op_change(2048)[4096]["add"], 0.28)

    def test_the_measured_multimap_keeps_its_default(self) -> None:
        sweep = sweep_of("multimap", MULTIMAP, MULTIMAP_BYTES)
        for mix in tune.MIXES:
            with self.subTest(mix=mix.name):
                d = sweep.decide(mix, 2048)
                self.assertEqual(d.why, "keep")
                self.assertEqual(d.nearest[0], 1024)
                self.assertGreater(d.nearest[1].median, 1 - tune.SPEED_WIN)

    def test_faster_in_every_run_is_a_win(self) -> None:
        win = {1024: {"a": 80.0}, 2048: {"a": 100.0}}
        d = sweep_of("", win).decide(tune.MIXES[0], 2048)
        self.assertEqual((d.why, d.limit), ("faster", 1024))
        self.assertAlmostEqual(d.versus.median, 0.8)
        # Exactly 15% faster still counts
        edge = sweep_of("", {1024: {"a": 85.0}, 2048: {"a": 100.0}})
        self.assertEqual(edge.decide(tune.MIXES[0], 2048).why, "faster")

    def test_one_slow_run_is_not_a_win(self) -> None:
        # 20% faster in the median, but one run only 10% faster
        sweep = tune.Sweep(
            "",
            {
                1024: {"a": [80.0, 80.0, 90.0]},
                2048: {"a": [100.0, 100.0, 100.0]},
            },
        )
        d = sweep.decide(tune.MIXES[0], 2048)
        self.assertEqual(d.why, "keep")
        self.assertEqual(d.nearest[0], 1024)
        self.assertEqual(
            d.blocked, "its slowest run is 0.900; a win needs 0.85 or less"
        )

    def test_no_win_makes_one_operation_15_percent_slower(self) -> None:
        # The mix is 30% faster, but b is 20% slower
        sweep = sweep_of(
            "", {1024: {"a": 40.0, "b": 120.0}, 2048: {"a": 100.0, "b": 100.0}}
        )
        d = sweep.decide(tune.MIXES[0], 2048)
        self.assertEqual(d.why, "keep")
        self.assertEqual(d.blocked, "b +20% there")

    def test_keep_names_the_densest_close_limit(self) -> None:
        # Against a 1024 baseline, 2048 holds 2% fewer bytes at 1.10: close,
        # but short of a memory win; 4096 is denser still but too slow
        sweep = sweep_of(
            "",
            {1024: {"a": 100.0}, 2048: {"a": 110.0}, 4096: {"a": 140.0}},
            {1024: 1000, 2048: 980, 4096: 900},
        )
        d = sweep.decide(tune.MIXES[0], 1024)
        self.assertEqual(d.why, "keep")
        self.assertEqual(d.denser[0], 2048)
        text = tune.decision_text("multimap", sweep, d)
        self.assertIn(
            "denser: 2048, 2.0% fewer bytes at 1.10 [1.10-1.10] (a memory win needs 5%)",
            text,
        )
        # Nothing denser than the baseline: no denser note
        self.assertIsNone(sweep.decide(tune.MIXES[0], 4096).denser)

    def test_keep_says_when_no_limit_is_faster(self) -> None:
        sweep = sweep_of("", {1024: {"a": 120.0}, 2048: {"a": 100.0}})
        self.assertEqual(
            sweep.decide(tune.MIXES[0], 2048).blocked,
            "no limit is faster; a +20% there",
        )

    def test_fewer_bytes_while_within_15_percent_is_a_win(self) -> None:
        ns = {1024: {"a": 100.0}, 2048: {"a": 100.0}, 4096: {"a": 110.0}}
        smaller = sweep_of("", ns, {1024: 1000, 2048: 1000, 4096: 940})
        d = smaller.decide(tune.MIXES[0], 2048)
        self.assertEqual((d.why, d.limit), ("smaller", 4096))
        # 4% fewer bytes is not enough
        slight = sweep_of("", ns, {1024: 1000, 2048: 1000, 4096: 960})
        self.assertEqual(slight.decide(tune.MIXES[0], 2048).why, "keep")
        # Nor is 6% fewer at 16% slower
        slower = sweep_of(
            "", {2048: {"a": 100.0}, 4096: {"a": 116.0}}, {2048: 1000, 4096: 940}
        )
        self.assertEqual(slower.decide(tune.MIXES[0], 2048).why, "keep")

    def test_among_wins_the_densest_is_named(self) -> None:
        sweep = sweep_of(
            "",
            {512: {"a": 60.0}, 1024: {"a": 70.0}, 2048: {"a": 100.0}},
            {512: 1100, 1024: 1050, 2048: 1000},
        )
        self.assertEqual(sweep.decide(tune.MIXES[0], 2048).limit, 1024)

    def test_a_mix_running_none_of_the_operations_decides_nothing(self) -> None:
        sweep = sweep_of("multimap", MULTIMAP)
        self.assertIsNone(sweep.decide(tune.parse_mix("getRank=1"), 2048))
        self.assertIsNone(sweep_of("", {}).decide(tune.MIXES[0], 2048))


class Report(unittest.TestCase):
    def test_render_reads_everything_against_the_baseline(self) -> None:
        found = {
            "multimap": sweep_of("multimap", MULTIMAP, MULTIMAP_BYTES),
            "multilist": sweep_of(
                "multilist",
                {1024: {"index": 300.0}, 2048: {"index": 400.0}},
            ),
        }
        mixes = tune.parse_mixes(["lookups:exists=90,insert=10"])
        text = "\n".join(tune.render(found, [("L1d", 128 << 10)], mixes, 2048))
        self.assertIn("caches    L1d 128 KB", text)
        self.assertIn("* marks the baseline, limit 2048", text)
        self.assertIn("lookups  exists 90%, insert 10%", text)
        self.assertIn("multimap (reads: exists; writes: insert, delete)", text)
        self.assertIn("run-to-run spread: insert ±2%, exists ±2%, delete ±2%", text)
        # The baseline row is marked; another row shows its changes
        self.assertRegex(text, r"\n\s+2048\* +771\.2 ")
        self.assertRegex(text, r"\n\s+1024 +728\.6 +-6% +527\.7 +-15%")
        self.assertRegex(text, r"2336757 +\+2\.0%")
        # read80 at 1024: 13% faster in every run, short of a clear win
        self.assertRegex(text, r"0\.87 \[0\.87-0\.87\]")
        self.assertIn(
            "read80   keep 2048: nearest is 1024 at 0.87 [0.87-0.87]: its slowest "
            "run is 0.866; a win needs 0.85 or less",
            text,
        )
        self.assertNotIn("[1.00-1.00]", text)  # the baseline row is just 1.00
        # multilist's index is 25% faster at 1024 in every run
        apply = "dkTierSetDefaultLimit(DK_TIER_MULTILIST, FLEX_CAP_LEVEL_1024);"
        self.assertIn(
            f"equal    change to 1024: 25% faster (0.75 [0.75-0.75]); apply: {apply}",
            text,
        )
        self.assertIn("lookups  runs none of these operations", text)
        self.assertNotIn("rough", text)

    def test_one_run_says_its_ranges_are_rough(self) -> None:
        found = {
            "multimap": tune.Sweep.of_medians(
                {1024: {"insert": 1.0}, 2048: {"insert": 1.0}}, family="multimap"
            )
        }
        text = "\n".join(tune.render(found, [], tune.MIXES, 2048))
        self.assertIn("only 1 run: the ranges and every-run checks are rough", text)
        self.assertNotIn("[1.00-", text)  # no ranges in the table

    def test_analysis_json(self) -> None:
        found = {"multiOrderedSet": sweep_of("multiOrderedSet", MOS, MOS_BYTES)}
        out = tune.analysis(found, tune.MIXES, 2048)
        self.assertEqual(out["baseline"], 2048)
        mos = out["families"]["multiOrderedSet"]
        self.assertEqual(mos["runs"], 5)
        self.assertEqual(mos["bytes"]["4096"], 7008639)
        self.assertAlmostEqual(mos["change"]["4096"]["add"], 1209.9 / 939.5 - 1, 3)
        equal = mos["mixes"]["equal"]
        self.assertEqual((equal["action"], equal["limit"]), ("keep", 2048))
        self.assertNotIn("apply", equal)
        self.assertEqual(
            equal["versus"]["2048"], {"median": 1.0, "low": 1.0, "high": 1.0}
        )

    def test_columns_line_up_whatever_the_change(self) -> None:
        # +1050% (a measured getRank) must not push the next column over
        sweep = sweep_of(
            "multiOrderedSet",
            {
                256: {"add": 870.5, "getRank": 23647.7},
                2048: {"add": 924.3, "getRank": 2056.9},
            },
            {256: 7391003, 2048: 7031527},
        )
        lines = tune.render({"multiOrderedSet": sweep}, [], tune.MIXES, 2048)
        start = next(
            i for i, line in enumerate(lines) if line.lstrip().startswith("limit")
        )
        header, wide, base = lines[start : start + 3]
        self.assertIn("+1050%", wide)
        # Byte counts end at one place on both rows, the header over the
        # whole cell (count and change), and the next column after it
        self.assertEqual(wide.index("7391003"), base.index("7031527"), (wide, base))
        self.assertEqual(header.index("bytes") + 5, wide.index("+5.1%") + 5)
        self.assertLess(wide.index("+5.1%") + 5, wide.index("8.22"))

    def test_reuse_reads_the_saved_runs_in_order(self) -> None:
        def tsv(path: Path, insert1024: float) -> None:
            rows = [
                ("multimapLimits", "insert1024", 2336757, insert1024),
                ("multimapLimits", "insert2048", 2291032, 100.0),
            ]
            path.write_text(
                "".join(f"PERF\t{m}\t{n}\t{b}\t1\t{ns}\t0\n" for m, n, b, ns in rows)
            )

        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            with self.assertRaises(ToolError):
                tune.load_runs(work)
            for r, ns in ((1, 80.0), (2, 82.0), (10, 84.0)):
                tsv(work / f"run-{r}.tsv", ns)
            runs = tune.load_runs(work)
            self.assertEqual(
                [
                    rows[("multimapLimits", "insert1024", 2336757)].ns_per_op
                    for rows in runs
                ],
                [80.0, 82.0, 84.0],
            )
            opts = tune.TuneOptions("x", families=["multimap"], reuse=True)
            self.assertEqual(tune.report(work, "title", runs, opts), 0)
            text = (work / "result.txt").read_text()
            self.assertTrue(text.startswith("title\n"))
            self.assertIn("equal    change to 1024: 18% faster", text)
            data = (work / "result.json").read_text()
            self.assertIn('"action": "change"', data)

    def test_the_baseline_must_be_measured(self) -> None:
        found = {"multimap": sweep_of("multimap", MULTIMAP)}
        tune.check_baseline(found, 2048)
        with self.assertRaises(ToolError):
            tune.check_baseline(found, 256)

    def test_the_default_baseline_is_the_compiled_default(self) -> None:
        header = (REPO / "src" / "dkTierLimit.h").read_text()
        self.assertIn(
            f"#define DK_TIER_LIMIT_COMPILED FLEX_CAP_LEVEL_{tune.DEFAULT_BASELINE}",
            header,
        )
        self.assertIn(
            "#define DK_TIER_LIMIT_ATOM_COMPILED "
            f"FLEX_CAP_LEVEL_{tune.DEFAULT_BASELINES['multimapAtom']}",
            header,
        )
        flexHeader = (REPO / "src" / "flex.h").read_text()
        self.assertIn(
            "#define FLEX_GROWTH_SLACK_PERCENT "
            f"{tune.DEFAULT_BASELINES['flexSlack']}\n",
            flexHeader,
        )
        source = (REPO / "src" / "multidict.c").read_text()
        self.assertIn(
            "MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR = "
            f"{tune.DEFAULT_BASELINES['multidict']};",
            source,
        )

    def test_each_family_reads_against_its_own_baseline(self) -> None:
        found = {
            "multimap": sweep_of("multimap", MULTIMAP, MULTIMAP_BYTES),
            "multidict": sweep_of("multidict", MULTIDICT, MULTIDICT_BYTES),
        }
        baselines = {f: tune.DEFAULT_BASELINES[f] for f in found}
        tune.check_baseline(found, baselines)
        with self.assertRaises(ToolError):
            tune.check_baseline(found, 2048)  # multidict never measured 2048
        text = "\n".join(tune.render(found, [], tune.MIXES, baselines))
        self.assertIn("each family's default (multimap 2048, multidict 800)", text)
        self.assertIn("800*", text)
        self.assertIn("2048*", text)
        out = tune.analysis(found, tune.MIXES, baselines)
        self.assertEqual(out["families"]["multidict"]["baseline"], 800)
        self.assertEqual(out["families"]["multimap"]["baseline"], 2048)

    def test_slower_lets_a_memory_win_cost_more_time(self) -> None:
        # Against 200, 800 holds 32% fewer bytes with finds about 31%
        # slower: past the default 15% cap on one operation, inside a 40%
        # one (3200's finds, over twice as slow, stay out either way)
        sweep = sweep_of("multidict", MULTIDICT, MULTIDICT_BYTES)
        equal = tune.MIXES[0]
        kept = sweep.decide(equal, 200)
        self.assertEqual(kept.why, "keep")
        taken = sweep.decide(equal, 200, slower=0.40)
        self.assertEqual((taken.why, taken.limit), ("smaller", 800))
        self.assertEqual(
            tune.decision_text("multidict", sweep, taken).split("apply: ")[1],
            "multidictSetDefaultLoadFactors(800, 40);",
        )
        legend = "\n".join(tune.legend(tune.MIXES, 200, 5, 0.40))
        self.assertIn("while under 40% slower", legend)


class Mixes(unittest.TestCase):
    def test_mix_weights_by_class_and_by_operation(self) -> None:
        ops = ["insert", "exists", "delete"]
        w = tune.MIXES[1].weights("multimap", ops)
        self.assertEqual(sorted(w), sorted(ops))
        self.assertAlmostEqual(w["exists"], 0.8)
        self.assertAlmostEqual(w["insert"], 0.1)
        self.assertAlmostEqual(w["delete"], 0.1)
        self.assertEqual(
            tune.MIXES[0].weights("multimap", ops), {op: 1 / 3 for op in ops}
        )
        # An operation's own share wins over its class's
        mix = tune.parse_mix("read=50,insert=50")
        self.assertEqual(mix.weights("multimap", ops), {"exists": 0.5, "insert": 0.5})
        mix = tune.parse_mix("exists=70,insert=20,delete=10")
        w = mix.weights("multimap", ops)
        self.assertAlmostEqual(w["exists"], 0.7)
        self.assertAlmostEqual(w["delete"], 0.1)
        # A mix of another family's operations runs none of these
        self.assertEqual(tune.parse_mix("getRank=1").weights("multimap", ops), {})

    def test_mixes_repeat_and_take_names(self) -> None:
        self.assertEqual(tune.parse_mixes(None), tune.MIXES)
        mixes = tune.parse_mixes(["read=9,write=1", "lookups:exists=1", "write=1"])
        self.assertEqual(
            [m.name for m in mixes],
            ["equal", "read80", "write80", "custom", "lookups", "custom2"],
        )
        self.assertEqual(mixes[4].shares, {"exists": 1.0})
        for bad in (["read80:read=1"], ["a:read=1", "a:write=1"], ["9x:read=1"]):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                tune.parse_mixes(bad)

    def test_bad_mixes_refuse(self) -> None:
        for bad in ("nope=1", "read", "read=x", "read=-1", "read=0", "write=inf"):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                tune.parse_mix(bad)
        self.assertIsNone(tune.parse_mix(None))

    def test_every_sweep_operation_is_a_read_or_a_write(self) -> None:
        source = (REPO / "src" / "perfRegression.c").read_text()
        rows: dict[str, set[str]] = {}
        for family, op in re.findall(
            r'PERF_LIMIT_ROW\(\s*"(\w+)Limits",\s*"(\w+)"', source
        ):
            rows.setdefault(family, set()).add(op)
        # multidict's sweep names its rows from its operation table
        names = re.search(r"mdLoadOpNames\[MD_LOAD_OP_COUNT\] = \{([^}]*)\}", source)
        self.assertIsNotNone(names)
        rows["multidict"] = set(re.findall(r'"(\w+)"', names.group(1)))
        self.assertIn('emitRow("multidictLimits"', source)
        # so does the flex growth slack's
        slack = re.search(
            r"flexSlackOpNames\[FLEX_SLACK_OP_COUNT\] = \{([^}]*)\}", source
        )
        self.assertIsNotNone(slack)
        rows["flexSlack"] = set(re.findall(r'"(\w+)"', slack.group(1)))
        self.assertIn('emitRow("flexSlackLimits"', source)
        self.assertEqual(sorted(rows), sorted(tune.FAMILIES))
        for family, ops in rows.items():
            with self.subTest(family=family):
                self.assertEqual(ops, set(tune.OP_CLASS[family]))
                self.assertLessEqual(
                    set(tune.OP_CLASS[family].values()), set(tune.CLASSES)
                )

    def test_apply_names_each_familys_setter(self) -> None:
        self.assertEqual(
            tune.apply_text("multiOrderedSet", 2048),
            "dkTierSetDefaultLimit(DK_TIER_MULTIORDEREDSET, FLEX_CAP_LEVEL_2048);",
        )
        self.assertEqual(
            tune.apply_text("multilist", 1024),
            "dkTierSetDefaultLimit(DK_TIER_MULTILIST, FLEX_CAP_LEVEL_1024);",
        )

    def test_every_family_has_a_setter_the_header_names(self) -> None:
        text = (REPO / "src" / "dkTierLimit.h").read_text()
        self.assertEqual(sorted(tune.DEFAULT_SETTERS), sorted(tune.TIER_FAMILIES))
        for family, setter in tune.DEFAULT_SETTERS.items():
            with self.subTest(family=family):
                self.assertIn(setter, text)
        # multidict's setting is its load factor, set by its own call
        self.assertIn(
            "bool multidictSetDefaultLoadFactors(",
            (REPO / "src" / "multidict.h").read_text(),
        )
        self.assertEqual(
            tune.apply_text("multidict", 1600),
            "multidictSetDefaultLoadFactors(1600, 80);",
        )
        # and flex's growth slack by flexSetGrowthSlack (0: no slack at all)
        self.assertIn("bool flexSetGrowthSlack(", (REPO / "src" / "flex.h").read_text())
        self.assertEqual(
            tune.apply_text("flexSlack", 6),
            "flexSetGrowthSlack(6, FLEX_GROWTH_SLACK_MIN_BYTES, "
            "FLEX_GROWTH_SLACK_MAX_BYTES);",
        )
        self.assertEqual(
            tune.apply_text("flexSlack", 0), "flexSetGrowthSlack(0, 0, 0);"
        )


class Caches(unittest.TestCase):
    def test_linux_sysfs(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for i, (level, kind, size) in enumerate(
                [
                    ("1", "Data", "48K"),
                    ("1", "Instruction", "32K"),
                    ("2", "Unified", "2048K"),
                ]
            ):
                index = root / f"index{i}"
                index.mkdir()
                (index / "level").write_text(level + "\n")
                (index / "type").write_text(kind + "\n")
                (index / "size").write_text(size + "\n")
            self.assertEqual(
                tune.cache_sizes(sys_root=root), [("L1d", 48 << 10), ("L2", 2 << 20)]
            )

    def test_sysctl(self) -> None:
        values = {"hw.perflevel0.l1dcachesize": "131072", "hw.l2cachesize": "4194304"}
        found = tune.cache_sizes(sysctl=values.get, sys_root=Path("/nonexistent-sysfs"))
        self.assertEqual(found, [("L1d (performance cores)", 131072), ("L2", 4194304)])


class Families(unittest.TestCase):
    def test_parse(self) -> None:
        self.assertEqual(tune.parse_families(None), list(tune.FAMILIES))
        self.assertEqual(tune.parse_families("multilist"), ["multilist"])
        for bad in ("nope", ",", "multimap,nope"):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                tune.parse_families(bad)


if __name__ == "__main__":
    unittest.main()

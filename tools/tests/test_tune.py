"""``dktools tune``: sweeps out of perfRegression rows, costs,
recommendations, cache sizes.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path

from dktools import tune
from dktools.gitutil import ToolError


def medians(rows: dict[tuple[str, str, int], float]) -> dict:
    return dict(rows)


class Sweeps(unittest.TestCase):
    def test_limit_rows_group_by_family_op_and_limit(self) -> None:
        found = tune.sweeps(
            medians(
                {
                    ("multimapLimits", "insert256", 2000): 700.0,
                    ("multimapLimits", "exists256", 0): 500.0,
                    ("multimapLimits", "insert1024", 1900): 690.0,
                    ("multimapLimits", "exists1024", 0): 480.0,
                    ("multilistLimits", "popHead2048", 0): 60.0,
                    ("multimap", "insert", 0): 1.0,  # not a sweep module
                    ("multimapLimits", "oddName", 0): 1.0,  # no limit suffix
                }
            )
        )
        self.assertEqual(sorted(found), ["multilist", "multimap"])
        mm = found["multimap"]
        self.assertEqual(mm.family, "multimap")
        self.assertEqual(mm.ns[256], {"insert": 700.0, "exists": 500.0})
        self.assertEqual(mm.bytes, {256: 2000, 1024: 1900})
        self.assertEqual(mm.ops(), ["insert", "exists"])

    def test_cost_is_the_mixs_mean_ns_over_its_lowest(self) -> None:
        sweep = tune.Sweep(
            ns={
                512: {"a": 100.0, "b": 200.0},  # mean 150
                1024: {"a": 200.0, "b": 100.0},  # mean 150
                2048: {"a": 100.0, "b": 100.0},  # mean 100
            }
        )
        costs = sweep.costs()
        self.assertAlmostEqual(costs[512], 1.5)
        self.assertAlmostEqual(costs[1024], 1.5)
        self.assertAlmostEqual(costs[2048], 1.0)
        self.assertEqual(sweep.recommend(), (2048, []))

    def test_read_and_write_mixes_pick_different_limits(self) -> None:
        # Small limits read fast and write slow; large ones the reverse
        sweep = tune.Sweep(
            family="multimap",
            ns={
                256: {"insert": 300.0, "exists": 100.0, "delete": 300.0},
                4096: {"insert": 100.0, "exists": 300.0, "delete": 100.0},
            },
            bytes={256: 1000, 4096: 1000},
        )
        read80, write80 = tune.MIXES[1], tune.MIXES[2]
        # read80: exists 0.8, insert and delete 0.1 each
        self.assertAlmostEqual(sweep.costs(read80)[4096], 260.0 / 140.0)
        self.assertEqual(sweep.recommend(read80), (256, []))
        self.assertEqual(sweep.recommend(write80), (4096, []))
        self.assertEqual(sweep.recommend(tune.MIXES[0]), (4096, []))

    def test_near_ties_are_listed(self) -> None:
        sweep = tune.Sweep(
            ns={
                512: {"a": 101.0},
                1024: {"a": 100.0},
                4096: {"a": 150.0},
            }
        )
        self.assertEqual(sweep.recommend(), (1024, [512]))

    def test_among_speed_ties_the_densest_wins(self) -> None:
        sweep = tune.Sweep(
            ns={256: {"a": 100.0}, 512: {"a": 101.0}, 1024: {"a": 101.5}},
            bytes={256: 2_560_000, 512: 2_430_000, 1024: 2_340_000},
        )
        self.assertEqual(sweep.recommend(), (1024, [256, 512]))

    def test_density_wins_below_a_fifteen_percent_slowdown(self) -> None:
        # The measured multimap shape: 1024 fastest, 2048 ~12% slower and
        # denser, 4096 densest but ~44% slower.
        sweep = tune.Sweep(
            ns={1024: {"a": 100.0}, 2048: {"a": 112.0}, 4096: {"a": 144.0}},
            bytes={1024: 2_244_129, 2048: 2_239_704, 4096: 2_237_182},
        )
        self.assertEqual(sweep.recommend(), (2048, [1024]))
        # 16% slower is a collapse, not a tie, however much denser
        edge = tune.Sweep(
            ns={1024: {"a": 100.0}, 2048: {"a": 116.0}},
            bytes={1024: 2, 2048: 1},
        )
        self.assertEqual(edge.recommend(), (1024, []))

    def test_render_explains_the_costs_and_recommends_per_mix(self) -> None:
        found = {
            "multimap": tune.Sweep(
                family="multimap",
                ns={256: {"insert": 2.0}, 512: {"insert": 1.0}},
                bytes={512: 99},
            )
        }
        custom = tune.parse_mix("exists=1")
        mixes = tune.mixes_with(custom)
        text = "\n".join(tune.render(found, [("L1d", 128 << 10)], mixes))
        self.assertIn("caches    L1d 128 KB", text)
        self.assertIn("mean ns per operation the mix runs", text)
        self.assertIn("read80   read 80%, write 20%", text)
        self.assertIn("(reads: -; writes: insert)", text)
        apply = "dkTierSetDefaultLimit(DK_TIER_MULTIMAP, FLEX_CAP_LEVEL_512);"
        for mix in ("equal", "read80", "write80"):
            self.assertIn(f"{mix:<8} recommends 512; apply: {apply}", text)
        # The custom mix only reads, and this sweep has no read
        self.assertIn("custom   runs none of these operations", text)
        rec = tune.recommendations(found, mixes)["multimap"]
        self.assertEqual(sorted(rec), ["equal", "read80", "write80"])
        self.assertEqual((rec["read80"]["limit"], rec["read80"]["apply"]), (512, apply))
        self.assertEqual(rec["read80"]["shares"], {"read": 80, "write": 20})

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

    def test_bad_mixes_refuse(self) -> None:
        for bad in ("nope=1", "read", "read=x", "read=-1", "read=0", "write=inf"):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                tune.parse_mix(bad)
        self.assertIsNone(tune.parse_mix(None))
        self.assertEqual(tune.mixes_with(None), tune.MIXES)

    def test_every_sweep_operation_is_a_read_or_a_write(self) -> None:
        source = (
            Path(__file__).resolve().parents[2] / "src" / "perfRegression.c"
        ).read_text()
        rows: dict[str, set[str]] = {}
        for family, op in re.findall(
            r'PERF_LIMIT_ROW\(\s*"(\w+)Limits",\s*"(\w+)"', source
        ):
            rows.setdefault(family, set()).add(op)
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
        header = Path(__file__).resolve().parents[2] / "src" / "dkTierLimit.h"
        text = header.read_text()
        self.assertEqual(sorted(tune.DEFAULT_SETTERS), sorted(tune.FAMILIES))
        for family, setter in tune.DEFAULT_SETTERS.items():
            with self.subTest(family=family):
                self.assertIn(setter, text)

    def test_empty_sweep_refuses(self) -> None:
        with self.assertRaises(ToolError):
            tune.Sweep().recommend()


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
